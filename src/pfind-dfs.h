#ifndef PFIND_DFS_H
#define PFIND_DFS_H

#include <stdint.h>
#include <time.h>
#include <mpi.h>
#include <daos.h>
#include <daos_fs.h>

/* Opaque directory handle owned by the backend's cache; never release it. */
typedef dfs_obj_t pfind_dir_t;

/* One entry returned by a scan. For DT_REG, size is valid only if size_valid. */
typedef struct {
  const char *name;
  unsigned char type;     /* DT_DIR, DT_REG, DT_LNK */
  int size_valid;
  int size_error;
  uint64_t size;
} pfind_entry_t;

/*
 * Per-batch callback. Return non-zero to abort the scan early (e.g. stonewall).
 * Called between pipeline RPCs, so the caller may do cheap MPI work in it.
 */
typedef int (*pfind_scan_cb_t)(pfind_entry_t *ents, uint32_t nr, uint64_t nr_scanned, void *arg);

void pfind_dfs_init(MPI_Comm comm);
void pfind_dfs_fini(MPI_Comm comm);

/* name_like: SQL LIKE pattern or NULL; newer: minimum ctime or 0 */
int pfind_dfs_set_filter(time_t newer, const char *name_like);

/* Resolve a directory path (cached). Returns 0 or errno. */
int pfind_dfs_dir_open(const char *path, pfind_dir_t **dir);

/* Number of independent enumeration splits (shards) of a directory. */
int pfind_dfs_dir_nsplits(pfind_dir_t *dir, uint32_t *nr);

/*
 * Scan one split of a directory through the server-side filter.
 * want_size: resolve file sizes (asynchronously, in batches) for DT_REG entries.
 * Returns 0 or errno.
 */
int pfind_dfs_scan_split(pfind_dir_t *dir, uint32_t split, int want_size,
                         pfind_scan_cb_t cb, void *arg);

#endif
