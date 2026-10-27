/*
 * DAOS DFS backend for pfind using the server-side pipeline filter.
 *
 * - Directory enumeration goes through dfs_readdir_with_filter(): the server
 *   evaluates name/ctime predicates and returns only matching entries (plus all
 *   directories so the tree can be traversed).
 * - A directory is scanned as independent splits (one per object shard group),
 *   so a single huge directory can be spread over many ranks.
 * - File sizes (-size) are resolved asynchronously with a per-rank event queue.
 * - Reply buffers are kept under the DAOS inline RPC limit so no bulk memory
 *   registration is needed per call (CXI MR resources are scarce at scale).
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <assert.h>

#include "pfind-dfs.h"

extern int pfind_rank;

#define DCHECK(rc, format, ...)                                                 \
  do {                                                                          \
    int _rc = (rc);                                                             \
    if (_rc != 0) {                                                             \
      fprintf(stderr, "ERROR (%s:%d): rank %d: rc %d: " format "\n", __FILE__,  \
              __LINE__, pfind_rank, _rc, ##__VA_ARGS__);                        \
      fflush(stderr);                                                           \
      MPI_Abort(MPI_COMM_WORLD, -1);                                            \
    }                                                                           \
  } while (0)

/*
 * Entries per pipeline RPC. With oid+chunk size requested the packed reply is
 * nr * (255 + 36 + 8) + headers; 64 keeps it below DAOS_BULK_LIMIT (19 KiB) so
 * the client never registers memory for a bulk transfer.
 */
#define SCAN_BATCH 64

/* directory handle cache (direct mapped) */
#define DIR_CACHE_BITS 12
#define DIR_CACHE_SIZE (1u << DIR_CACHE_BITS)

/*
 * daos/object.h: DIOF_TO_SPEC_SHARD. dfs_obj_anchor_set() only tags the anchor
 * with DIOF_TO_SPEC_GROUP; without the SHARD flag the pipeline client advances
 * to the next shard on EOF and one "split" ends up walking the whole object.
 */
#define ANCHOR_FLAG_TO_SPEC_SHARD 0x2

struct dir_cache_ent {
  char *path;
  dfs_obj_t *obj;
};

static daos_handle_t poh;
static daos_handle_t coh;
static dfs_t *dfs;
static dfs_pipeline_t *dpipe;
static daos_handle_t eq;
static daos_size_t default_chunk_size;
static struct dir_cache_ent dir_cache[DIR_CACHE_SIZE];

/* async size query state, one per entry of a batch */
struct size_req {
  daos_handle_t oh;
  daos_event_t ev;
  daos_size_t size;
  int idx;          /* index into the batch entry array */
};

enum handleType { POOL_HANDLE, CONT_HANDLE, DFS_HANDLE };

static void HandleDistribute(MPI_Comm comm, enum handleType type){
  d_iov_t global = { .iov_buf = NULL, .iov_buf_len = 0, .iov_len = 0 };
  int rc;

  if (pfind_rank == 0) {
    if (type == POOL_HANDLE)
      rc = daos_pool_local2global(poh, &global);
    else if (type == CONT_HANDLE)
      rc = daos_cont_local2global(coh, &global);
    else
      rc = dfs_local2global(dfs, &global);
    DCHECK(rc, "Failed to get global handle size");
  }

  MPI_Bcast(&global.iov_buf_len, 1, MPI_UINT64_T, 0, comm);

  global.iov_len = global.iov_buf_len;
  global.iov_buf = malloc(global.iov_buf_len);
  if (global.iov_buf == NULL)
    MPI_Abort(comm, -1);

  if (pfind_rank == 0) {
    if (type == POOL_HANDLE)
      rc = daos_pool_local2global(poh, &global);
    else if (type == CONT_HANDLE)
      rc = daos_cont_local2global(coh, &global);
    else
      rc = dfs_local2global(dfs, &global);
    DCHECK(rc, "Failed to create global handle");
  }

  MPI_Bcast(global.iov_buf, global.iov_buf_len, MPI_BYTE, 0, comm);

  if (pfind_rank != 0) {
    if (type == POOL_HANDLE)
      rc = daos_pool_global2local(global, &poh);
    else if (type == CONT_HANDLE)
      rc = daos_cont_global2local(poh, global, &coh);
    else
      rc = dfs_global2local(poh, coh, O_RDONLY, global, &dfs);
    DCHECK(rc, "Failed to get local handle");
  }

  free(global.iov_buf);
}

void pfind_dfs_init(MPI_Comm comm){
  int rc;

  char *pool_str = getenv("DAOS_POOL");
  char *cont_str = getenv("DAOS_CONT");
  if (pool_str == NULL || cont_str == NULL) {
    if (pfind_rank == 0)
      fprintf(stderr, "DAOS_POOL and DAOS_CONT env variables must be set\n");
    MPI_Abort(comm, -1);
  }

  rc = daos_init();
  DCHECK(rc, "Failed to initialize daos");

  if (pfind_rank == 0) {
    rc = daos_pool_connect(pool_str, NULL, DAOS_PC_RO, &poh, NULL, NULL);
    DCHECK(rc, "Failed to connect to pool %s", pool_str);
    rc = daos_cont_open(poh, cont_str, DAOS_COO_RO, &coh, NULL, NULL);
    DCHECK(rc, "Failed to open container %s", cont_str);
    rc = dfs_mount(poh, coh, O_RDONLY, &dfs);
    DCHECK(rc, "Failed to mount DFS namespace");
  }

  HandleDistribute(comm, POOL_HANDLE);
  HandleDistribute(comm, CONT_HANDLE);
  HandleDistribute(comm, DFS_HANDLE);

  char *dfs_prefix = getenv("DAOS_PREFIX");
  if (dfs_prefix != NULL) {
    rc = dfs_set_prefix(dfs, dfs_prefix);
    DCHECK(rc, "Failed to set DFS prefix %s", dfs_prefix);
  }

  dfs_attr_t attr = {0};
  rc = dfs_query(dfs, &attr);
  DCHECK(rc, "dfs_query failed");
  default_chunk_size = attr.da_chunk_size;

  rc = daos_eq_create(&eq);
  DCHECK(rc, "Failed to create event queue");

  memset(dir_cache, 0, sizeof(dir_cache));
}

void pfind_dfs_fini(MPI_Comm comm){
  int rc;

  for (unsigned i = 0; i < DIR_CACHE_SIZE; i++) {
    if (dir_cache[i].obj) {
      dfs_release(dir_cache[i].obj);
      free(dir_cache[i].path);
      dir_cache[i].obj = NULL;
      dir_cache[i].path = NULL;
    }
  }

  if (dpipe) {
    rc = dfs_pipeline_destroy(dpipe);
    DCHECK(rc, "Failed to destroy pipeline");
    dpipe = NULL;
  }

  rc = daos_eq_destroy(eq, 0);
  DCHECK(rc, "Failed to destroy event queue");

  rc = dfs_umount(dfs);
  DCHECK(rc, "Failed to umount DFS namespace");
  MPI_Barrier(comm);

  rc = daos_cont_close(coh, NULL);
  DCHECK(rc, "Failed to close container");
  MPI_Barrier(comm);

  rc = daos_pool_disconnect(poh, NULL);
  DCHECK(rc, "Failed to disconnect from pool");
  MPI_Barrier(comm);

  rc = daos_fini();
  DCHECK(rc, "Failed to finalize DAOS");
}

int pfind_dfs_set_filter(time_t newer, const char *name_like){
  dfs_predicate_t pred;
  uint64_t flags = 0;
  int rc;

  memset(&pred, 0, sizeof(pred));
  if (newer) {
    flags |= DFS_FILTER_NEWER;
    pred.dp_newer = newer;
  }
  if (name_like) {
    if (strlen(name_like) >= DFS_MAX_NAME)
      return ENAMETOOLONG;
    flags |= DFS_FILTER_NAME;
    strcpy(pred.dp_name, name_like);
  }

  rc = dfs_pipeline_create(dfs, pred, flags, &dpipe);
  if (rc)
    fprintf(stderr, "rank %d: dfs_pipeline_create failed: %d\n", pfind_rank, rc);
  return rc;
}

static uint32_t path_hash(const char *s){
  uint32_t h = 2166136261u;
  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= 16777619u;
  }
  return h;
}

int pfind_dfs_dir_open(const char *path, pfind_dir_t **dir){
  struct dir_cache_ent *e = &dir_cache[path_hash(path) & (DIR_CACHE_SIZE - 1)];
  dfs_obj_t *obj;
  mode_t mode;
  int rc;

  if (e->obj && strcmp(e->path, path) == 0) {
    *dir = e->obj;
    return 0;
  }

  rc = dfs_lookup(dfs, path, O_RDONLY, &obj, &mode, NULL);
  if (rc)
    return rc;
  if (!S_ISDIR(mode)) {
    dfs_release(obj);
    return ENOTDIR;
  }

  /* direct mapped: evict whatever was in this slot */
  if (e->obj) {
    dfs_release(e->obj);
    free(e->path);
  }
  e->obj = obj;
  e->path = strdup(path);
  if (e->path == NULL) {
    dfs_release(obj);
    e->obj = NULL;
    return ENOMEM;
  }
  *dir = obj;
  return 0;
}

int pfind_dfs_dir_nsplits(pfind_dir_t *dir, uint32_t *nr){
  *nr = 0;
  return dfs_obj_anchor_split(dir, nr, NULL);
}

/* resolve sizes for all DT_REG entries of a batch concurrently */
static void resolve_sizes(pfind_entry_t *ents, daos_obj_id_t *oids, daos_size_t *csizes,
                          uint32_t nr){
  struct size_req reqs[SCAN_BATCH];
  daos_event_t *evp[SCAN_BATCH];
  int inflight = 0;
  int rc;

  for (uint32_t i = 0; i < nr; i++) {
    if (ents[i].type != DT_REG)
      continue;
    struct size_req *r = &reqs[inflight];
    r->idx = i;
    r->size = 0;
    rc = daos_array_open_with_attr(coh, oids[i], DAOS_TX_NONE, DAOS_OO_RO, 1,
                                   csizes[i] ? csizes[i] : default_chunk_size, &r->oh, NULL);
    if (rc) {
      ents[i].size_error = 1;
      continue;
    }
    rc = daos_event_init(&r->ev, eq, NULL);
    if (rc == 0)
      rc = daos_array_get_size(r->oh, DAOS_TX_NONE, &r->size, &r->ev);
    if (rc) {
      daos_event_fini(&r->ev);
      daos_array_close(r->oh, NULL);
      ents[i].size_error = 1;
      continue;
    }
    inflight++;
  }

  int done = 0;
  while (done < inflight) {
    rc = daos_eq_poll(eq, 1, DAOS_EQ_WAIT, SCAN_BATCH, evp);
    if (rc < 0) {
      fprintf(stderr, "rank %d: daos_eq_poll failed: %d\n", pfind_rank, rc);
      break;
    }
    done += rc;
  }

  for (int j = 0; j < inflight; j++) {
    struct size_req *r = &reqs[j];
    pfind_entry_t *ent = &ents[r->idx];
    if (done < inflight || r->ev.ev_error) {
      ent->size_error = 1;
    } else {
      ent->size = r->size;
      ent->size_valid = 1;
    }
    daos_event_fini(&r->ev);
    daos_array_close(r->oh, NULL);
  }
}

int pfind_dfs_scan_split(pfind_dir_t *dir, uint32_t split, int want_size,
                         pfind_scan_cb_t cb, void *arg){
  daos_anchor_t anchor;
  struct dirent dirs[SCAN_BATCH];
  daos_obj_id_t oids[SCAN_BATCH];
  daos_size_t csizes[SCAN_BATCH];
  pfind_entry_t ents[SCAN_BATCH];
  int rc;

  rc = dfs_obj_anchor_set(dir, split, &anchor);
  if (rc)
    return rc;
  anchor.da_flags |= ANCHOR_FLAG_TO_SPEC_SHARD;

  while (!daos_anchor_is_eof(&anchor)) {
    uint32_t nr = SCAN_BATCH;
    uint64_t nr_scanned = 0;

    rc = dfs_readdir_with_filter(dfs, dir, dpipe, &anchor, &nr, dirs, oids, csizes, &nr_scanned);
    if (rc)
      return rc;

    for (uint32_t i = 0; i < nr; i++) {
      ents[i].name = dirs[i].d_name;
      ents[i].type = dirs[i].d_type;
      ents[i].size_valid = 0;
      ents[i].size_error = 0;
      ents[i].size = 0;
    }

    if (want_size && nr > 0)
      resolve_sizes(ents, oids, csizes, nr);

    if (cb(ents, nr, nr_scanned, arg))
      break;
  }
  return 0;
}
