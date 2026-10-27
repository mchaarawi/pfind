/*
 * pfind core for the DAOS DFS pipeline backend, designed for O(100K) ranks.
 *
 * Work items are (directory path, split range). A directory is first split into
 * its enumeration shards; ranges are bisected on processing so a huge directory
 * spreads across ranks in O(log n) steal steps. The local queue grows on demand
 * (no overflow side queue).
 *
 * Stealing is non-blocking: a thief posts one request and keeps serving other
 * ranks and progressing termination while waiting; victims give half their
 * queue. Failed steals back off exponentially so idle ranks don't flood the
 * fabric.
 *
 * Termination uses Mattern-style counting waves over MPI_Iallreduce (sent/received
 * item counters + busy/dirty flags) instead of a ring token, so it costs
 * O(log N) per wave regardless of how many ranks are idle or busy.
 */
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <assert.h>
#include <regex.h>
#include <pthread.h>
#include <time.h>
#include <mpi.h>

#include "pfind-options.h"
#include "pfind-dfs.h"

#define PFIND_PATH_MAX 256

#define TAG_STEAL_REQ  800
#define TAG_STEAL_RESP 820

#define MAX_GIVE            4096      /* items per steal response */
#define STEAL_BACKOFF_MIN   20e-6     /* seconds */
#define STEAL_BACKOFF_MAX   5e-3
#define TERM_WAVE_INTERVAL  10e-3
#define TERM_FIELDS         6

/* debugging knobs (seconds, 0 disables): PFIND_PROGRESS (rank 0), PFIND_WATCHDOG (any rank) */
#define DEFAULT_PROGRESS_SEC 10
#define DEFAULT_WATCHDOG_SEC 30

int pfind_rank;
int pfind_size;
MPI_Comm pfind_com;

#define CHECK_MPI(ret) do { if ((ret) != MPI_SUCCESS) { fprintf(stderr, "rank %d: MPI error at %s:%d\n", pfind_rank, __FILE__, __LINE__); MPI_Abort(MPI_COMM_WORLD, 1); } } while (0)

typedef struct {
  uint32_t lo;
  uint32_t hi;                  /* hi == 0: directory not split yet */
  char path[PFIND_PATH_MAX];
} work_t;

typedef struct {
  uint64_t ctime_min;
  double stonewall_endtime;
  int stonewall_hit;
  FILE *logfile;
  int needs_size;
  int client_regex;             /* -regex given: name match done on the client */
} pfind_runtime_t;

static pfind_options_t *opt;
static pfind_runtime_t runtime;
static pfind_find_results_t *res;
static int printed_warning;
static double t_start;

/* ------------------------------------------------------------ watchdog */

static double mono_now(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

enum { WD_IDLE = 0, WD_OPEN, WD_SPLIT, WD_SCAN };
static const char *wd_names[] = { "idle", "dir_open", "nsplits", "scan" };
static volatile int wd_state;
static volatile double wd_op_start;
static volatile uint64_t wd_batches;
static volatile uint32_t wd_split;
static char wd_path[PFIND_PATH_MAX];
static volatile int wd_stop;
static volatile int wd_steal_victim = -1;
static volatile double wd_steal_start;
static int wd_interval;
static int progress_interval;
static pthread_t wd_thread;

static void wd_set(int state, const char *path, uint32_t split){
  if (path != wd_path)
    snprintf(wd_path, sizeof(wd_path), "%s", path);
  wd_split = split;
  wd_batches = 0;
  wd_op_start = mono_now();
  wd_state = state;
}

static void *watchdog_main(void *arg){
  (void)arg;
  double last_op_report = 0, last_steal_report = 0;
  while (!wd_stop) {
    struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
    double now = mono_now();
    int st = wd_state;
    if (st != WD_IDLE && now - wd_op_start > wd_interval && now - last_op_report > wd_interval) {
      fprintf(stderr, "[pfind wd] rank %d: %s '%s' split %u running for %.0fs (%lu batches done)\n",
              pfind_rank, wd_names[st], wd_path, wd_split, now - wd_op_start,
              (unsigned long)wd_batches);
      last_op_report = now;
    }
    int v = wd_steal_victim;
    if (v >= 0 && now - wd_steal_start > wd_interval && now - last_steal_report > wd_interval) {
      fprintf(stderr, "[pfind wd] rank %d: steal request to rank %d unanswered for %.0fs\n",
              pfind_rank, v, now - wd_steal_start);
      last_steal_report = now;
    }
  }
  return NULL;
}

static int env_seconds(const char *name, int def){
  char *s = getenv(name);
  return s ? atoi(s) : def;
}

/* ---------------------------------------------------------------- queue */

static work_t *q;
static size_t q_cap, q_head, q_tail;    /* live items in [q_head, q_tail) */

static void *smalloc(size_t size){
  void *p = malloc(size);
  if (p == NULL) {
    fprintf(stderr, "rank %d: cannot allocate %zu bytes\n", pfind_rank, size);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  memset(p, 0, size);
  return p;
}

static inline size_t q_count(void){
  return q_tail - q_head;
}

static void q_reserve(size_t extra){
  if (q_tail + extra <= q_cap)
    return;
  size_t n = q_count();
  if (q_head > 0) {
    memmove(q, q + q_head, n * sizeof(work_t));
    q_head = 0;
    q_tail = n;
    if (q_tail + extra <= q_cap)
      return;
  }
  size_t ncap = q_cap ? q_cap : 1024;
  while (ncap < n + extra)
    ncap *= 2;
  work_t *nq = realloc(q, ncap * sizeof(work_t));
  if (nq == NULL) {
    fprintf(stderr, "rank %d: cannot grow work queue to %zu items\n", pfind_rank, ncap);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  q = nq;
  q_cap = ncap;
}

static void q_push(const work_t *w){
  q_reserve(1);
  q[q_tail++] = *w;
}

static int q_pop(work_t *w){
  if (q_count() == 0)
    return 0;
  *w = q[--q_tail];
  if (q_tail == q_head)
    q_head = q_tail = 0;
  return 1;
}

/* remove k items from the steal end (oldest, i.e. highest in the tree) */
static void q_take_front(size_t k, work_t *dst){
  memcpy(dst, q + q_head, k * sizeof(work_t));
  q_head += k;
  if (q_head == q_tail)
    q_head = q_tail = 0;
}

static void q_clear(void){
  q_head = q_tail = 0;
}

/* ------------------------------------------------------- steal protocol */

/* counters for termination detection: items transferred between ranks */
static uint64_t items_sent;
static uint64_t items_recv;
static int dirty;                       /* received items since last wave report */

struct resp_send {
  MPI_Request req;
  work_t *buf;
  struct resp_send *next;
};
static struct resp_send *resp_list;

static int steal_victim = -1;           /* rank we have a request pending to */
static MPI_Request steal_req = MPI_REQUEST_NULL;
static double steal_start;
static int failed_steals;
static double backoff_until;
static unsigned int rand_seed;

static void reap_resp_sends(int wait_all){
  struct resp_send **pp = &resp_list;
  while (*pp) {
    struct resp_send *r = *pp;
    int done = 0;
    if (wait_all) {
      CHECK_MPI(MPI_Wait(&r->req, MPI_STATUS_IGNORE));
      done = 1;
    } else {
      CHECK_MPI(MPI_Test(&r->req, &done, MPI_STATUS_IGNORE));
    }
    if (done) {
      *pp = r->next;
      free(r->buf);
      free(r);
    } else {
      pp = &r->next;
    }
  }
}

static void serve_steal_requests(void){
  while (1) {
    int flag;
    MPI_Status st;
    CHECK_MPI(MPI_Iprobe(MPI_ANY_SOURCE, TAG_STEAL_REQ, pfind_com, &flag, &st));
    if (!flag)
      return;
    int src = st.MPI_SOURCE;
    CHECK_MPI(MPI_Recv(NULL, 0, MPI_BYTE, src, TAG_STEAL_REQ, pfind_com, MPI_STATUS_IGNORE));
    res->monitor.job_steal_inbound++;

    size_t give = runtime.stonewall_hit ? 0 : q_count() / 2;
    if (give > MAX_GIVE)
      give = MAX_GIVE;

    struct resp_send *r = smalloc(sizeof(*r));
    if (give) {
      r->buf = smalloc(give * sizeof(work_t));
      q_take_front(give, r->buf);
    }
    CHECK_MPI(MPI_Isend(r->buf, give * sizeof(work_t), MPI_BYTE, src, TAG_STEAL_RESP, pfind_com, &r->req));
    r->next = resp_list;
    resp_list = r;
    items_sent += give;
    res->monitor.work_send += give;
  }
}

static void progress_steal(void){
  int flag, bytes;
  MPI_Status st;

  if (steal_victim < 0)
    return;
  CHECK_MPI(MPI_Iprobe(steal_victim, TAG_STEAL_RESP, pfind_com, &flag, &st));
  if (!flag)
    return;
  CHECK_MPI(MPI_Get_count(&st, MPI_BYTE, &bytes));
  size_t n = bytes / sizeof(work_t);
  q_reserve(n);
  CHECK_MPI(MPI_Recv(q + q_tail, bytes, MPI_BYTE, steal_victim, TAG_STEAL_RESP, pfind_com, MPI_STATUS_IGNORE));
  if (!runtime.stonewall_hit)
    q_tail += n;
  items_recv += n;
  CHECK_MPI(MPI_Wait(&steal_req, MPI_STATUS_IGNORE));

  double now = MPI_Wtime();
  res->monitor.job_steal_mpitime_us += (uint64_t)((now - steal_start) * 1e6);
  if (n > 0) {
    dirty = 1;
    failed_steals = 0;
    backoff_until = 0;
    res->monitor.work_stolen += n;
  } else {
    double b = STEAL_BACKOFF_MIN * (double)(1u << (failed_steals > 12 ? 12 : failed_steals));
    if (b > STEAL_BACKOFF_MAX)
      b = STEAL_BACKOFF_MAX;
    backoff_until = now + b;
    failed_steals++;
  }
  steal_victim = -1;
  wd_steal_victim = -1;
}

static void maybe_steal(double now){
  if (pfind_size == 1 || steal_victim >= 0 || q_count() > 0 || runtime.stonewall_hit)
    return;
  if (now < backoff_until)
    return;

  int victim;
  if (opt->steal_from_next && pfind_rank > 0 && (rand_r(&rand_seed) & 1))
    victim = pfind_rank - 1;
  else
    victim = (int)(rand_r(&rand_seed) % (unsigned)pfind_size);
  if (victim == pfind_rank)
    victim = (victim + 1) % pfind_size;

  CHECK_MPI(MPI_Isend(NULL, 0, MPI_BYTE, victim, TAG_STEAL_REQ, pfind_com, &steal_req));
  steal_victim = victim;
  steal_start = now;
  wd_steal_start = mono_now();
  wd_steal_victim = victim;
  res->monitor.job_steal_tries++;
}

/* --------------------------------------------------- termination waves */

static MPI_Request term_req = MPI_REQUEST_NULL;
static uint64_t term_in[TERM_FIELDS], term_out[TERM_FIELDS], term_last_out[TERM_FIELDS];
static uint64_t term_prev_sent, term_prev_recv;
static int term_have_prev;
static double term_last;
static uint64_t waves_completed;
static double last_progress;

/* rank 0 only: periodic one-line status built from the last completed wave */
static void progress_report(int final){
  if (pfind_rank != 0 || !progress_interval)
    return;
  double now = MPI_Wtime();
  if (!final && now - last_progress < progress_interval)
    return;
  last_progress = now;
  fprintf(stderr, "[pfind] %s t=%.0fs waves=%lu last-wave: busy=%lu dirty=%lu steals-pending=%lu sent=%lu recv=%lu%s%lu"
          " | rank0: q=%zu found=%lu scanned=%lu state=%s\n",
          final ? "DONE" : "progress", now - t_start, (unsigned long)waves_completed,
          (unsigned long)term_last_out[2], (unsigned long)term_last_out[3],
          (unsigned long)term_last_out[4], (unsigned long)term_last_out[0],
          (unsigned long)term_last_out[1],
          term_last_out[2] == 1 ? " busy-rank=" : " busy-rank-sum=", (unsigned long)term_last_out[5],
          q_count(), (unsigned long)res->found_files, (unsigned long)res->total_files,
          wd_names[wd_state]);
  fflush(stderr);
}

/* returns 1 when global termination has been detected */
static int progress_termination(int busy, double now){
  int done;

  if (term_req == MPI_REQUEST_NULL) {
    if (now - term_last < TERM_WAVE_INTERVAL)
      return 0;
    term_in[0] = items_sent;
    term_in[1] = items_recv;
    term_in[2] = busy ? 1 : 0;
    term_in[3] = dirty ? 1 : 0;
    term_in[4] = steal_victim >= 0 ? 1 : 0;
    term_in[5] = busy ? (uint64_t)pfind_rank : 0;
    dirty = 0;
    term_last = now;
    CHECK_MPI(MPI_Iallreduce(term_in, term_out, TERM_FIELDS, MPI_UINT64_T, MPI_SUM, pfind_com, &term_req));
    res->monitor.completion_tokens_send++;
    return 0;
  }

  CHECK_MPI(MPI_Test(&term_req, &done, MPI_STATUS_IGNORE));
  if (!done)
    return 0;

  memcpy(term_last_out, term_out, sizeof(term_out));
  waves_completed++;
  progress_report(0);
  int quiet = term_out[2] == 0 && term_out[3] == 0 && term_out[0] == term_out[1];
  int stable = term_have_prev && term_out[0] == term_prev_sent && term_out[1] == term_prev_recv;
  term_prev_sent = term_out[0];
  term_prev_recv = term_out[1];
  term_have_prev = 1;
  return quiet && stable;
}

/* -------------------------------------------------------- processing */

struct scan_ctx {
  const char *path;
  size_t path_len;
};

static int name_matches(const char *name){
  if (!runtime.client_regex)
    return 1;
  return regexec(&opt->name_regex, name, 0, NULL, 0) == 0;
}

static int scan_cb(pfind_entry_t *ents, uint32_t nr, uint64_t nr_scanned, void *arg){
  struct scan_ctx *ctx = arg;

  res->checked_dirents += nr_scanned;
  res->total_files += nr_scanned;

  for (uint32_t i = 0; i < nr; i++) {
    pfind_entry_t *e = &ents[i];

    if (e->type == DT_DIR) {
      res->total_files--;
      if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0)
        continue;
      work_t w = { .lo = 0, .hi = 0 };
      int len = snprintf(w.path, sizeof(w.path), "%s%s%s", ctx->path,
                         ctx->path_len && ctx->path[ctx->path_len - 1] == '/' ? "" : "/", e->name);
      if (len < 0 || (size_t)len >= sizeof(w.path)) {
        res->errors++;
        if (!printed_warning) {
          fprintf(stderr, "rank %d: path too long (>%d), skipping %s/%s (further messages suppressed)\n",
                  pfind_rank, PFIND_PATH_MAX, ctx->path, e->name);
          printed_warning = 1;
        }
        continue;
      }
      q_push(&w);
      continue;
    }

    if (!name_matches(e->name))
      continue;

    if (runtime.needs_size) {
      if (e->type != DT_REG)
        continue;
      if (e->size_error) {
        res->errors++;
        continue;
      }
      if (e->size != opt->size)
        continue;
    } else if (e->type != DT_REG && e->type != DT_LNK) {
      res->unknown_file++;
      continue;
    }

    res->found_files++;
    if (!opt->just_count)
      fprintf(runtime.logfile, "%s/%s\n", ctx->path, e->name);
  }

  /* keep other ranks moving while this split is being scanned */
  wd_batches++;
  serve_steal_requests();
  reap_resp_sends(0);
  progress_termination(1, MPI_Wtime());
  progress_report(0);

  return opt->stonewall_timer && MPI_Wtime() >= runtime.stonewall_endtime;
}

static void process_item(work_t *w){
  pfind_dir_t *dir;
  int rc;

  wd_set(WD_OPEN, w->path, w->lo);
  rc = pfind_dfs_dir_open(w->path, &dir);
  if (rc) {
    wd_state = WD_IDLE;
    res->errors++;
    if (opt->verbosity > 1)
      fprintf(runtime.logfile, "Cannot open '%s': %s\n", w->path, strerror(rc));
    return;
  }

  if (w->hi == 0) {
    uint32_t nr = 0;
    wd_set(WD_SPLIT, wd_path, 0);
    rc = pfind_dfs_dir_nsplits(dir, &nr);
    if (rc || nr == 0) {
      wd_state = WD_IDLE;
      res->errors++;
      fprintf(stderr, "rank %d: cannot split '%s': %s\n", pfind_rank, w->path, strerror(rc ? rc : EINVAL));
      return;
    }
    w->lo = 0;
    w->hi = nr;
  }

  /* bisect: keep the lowest split, queue the rest in halves for thieves */
  while (w->hi - w->lo > 1) {
    uint32_t mid = w->lo + (w->hi - w->lo) / 2;
    work_t rest = *w;
    rest.lo = mid;
    q_push(&rest);
    w->hi = mid;
  }

  struct scan_ctx ctx = { .path = w->path, .path_len = strlen(w->path) };
  wd_set(WD_SCAN, wd_path, w->lo);
  rc = pfind_dfs_scan_split(dir, w->lo, runtime.needs_size, scan_cb, &ctx);
  wd_state = WD_IDLE;
  if (rc) {
    res->errors++;
    fprintf(stderr, "rank %d: scan of '%s' split %u failed: %s\n", pfind_rank, w->path, w->lo, strerror(rc));
  }
}

/* ------------------------------------------------------------- driver */

pfind_find_results_t *pfind_find(pfind_options_t *lopt){
  opt = lopt;
  memset(&runtime, 0, sizeof(runtime));

  if (opt->delete_files || opt->delete_dirs || opt->uid != (uid_t)-1 || opt->gid != (uid_t)-1) {
    if (pfind_rank == 0)
      fprintf(stderr, "pfind-dfs: -e/-E/-uid/-gid are not supported by the DFS backend\n");
    MPI_Abort(pfind_com, 1);
  }

  pfind_dfs_init(pfind_com);

  if (opt->timestamp_file) {
    if (pfind_rank == 0) {
      struct stat timer_file;
      if (lstat(opt->timestamp_file, &timer_file) != 0) {
        fprintf(stderr, "Could not open: \"%s\", error: %s\n", opt->timestamp_file, strerror(errno));
        MPI_Abort(pfind_com, 1);
      }
      runtime.ctime_min = timer_file.st_ctime;
    }
    MPI_Bcast(&runtime.ctime_min, 1, MPI_UINT64_T, 0, pfind_com);
  }

  if (opt->results_dir && !opt->just_count) {
    char outfile[PATH_MAX];
    snprintf(outfile, sizeof(outfile), "%s/%d.txt", opt->results_dir, pfind_rank);
    mkdir(opt->results_dir, S_IRWXU);
    runtime.logfile = fopen(outfile, "w");
    if (runtime.logfile == NULL) {
      fprintf(stderr, "rank %d: could not open output logfile %s\n", pfind_rank, outfile);
      MPI_Abort(pfind_com, 1);
    }
  } else {
    runtime.logfile = stdout;
  }

  runtime.needs_size = opt->size != UINT64_MAX;
  runtime.client_regex = opt->name_pattern != NULL && opt->name_like == NULL;

  int rc = pfind_dfs_set_filter(runtime.ctime_min, opt->name_like);
  if (rc)
    MPI_Abort(pfind_com, 1);

  res = smalloc(sizeof(*res));
  q_cap = 0;
  q = NULL;
  q_reserve(opt->queue_length > 0 ? (size_t)opt->queue_length : 1024);
  rand_seed = (unsigned)pfind_rank * 2654435761u + 12345u;

  items_sent = items_recv = 0;
  dirty = 0;
  term_have_prev = 0;
  term_last = 0;
  failed_steals = 0;
  backoff_until = 0;

  if (opt->stonewall_timer)
    runtime.stonewall_endtime = MPI_Wtime() + opt->stonewall_timer;

  progress_interval = env_seconds("PFIND_PROGRESS", DEFAULT_PROGRESS_SEC);
  wd_interval = env_seconds("PFIND_WATCHDOG", DEFAULT_WATCHDOG_SEC);
  wd_state = WD_IDLE;
  wd_stop = 0;
  waves_completed = 0;
  memset(term_last_out, 0, sizeof(term_last_out));
  if (wd_interval > 0 && pthread_create(&wd_thread, NULL, watchdog_main, NULL) != 0) {
    fprintf(stderr, "rank %d: could not start watchdog thread, continuing without\n", pfind_rank);
    wd_interval = 0;
  }

  MPI_Barrier(pfind_com);
  double start = MPI_Wtime();
  t_start = start;
  last_progress = start;
  if (pfind_rank == 0 && progress_interval)
    fprintf(stderr, "[pfind] start: %d ranks, workdir '%s', like='%s' newer=%lu size=%s progress=%ds watchdog=%ds\n",
            pfind_size, opt->workdir, opt->name_like ? opt->name_like : "",
            (unsigned long)runtime.ctime_min, runtime.needs_size ? "yes" : "no",
            progress_interval, wd_interval);

  if (pfind_rank == 0) {
    work_t root = { .lo = 0, .hi = 0 };
    if (strlen(opt->workdir) >= sizeof(root.path)) {
      fprintf(stderr, "workdir path too long (>%d)\n", PFIND_PATH_MAX);
      MPI_Abort(pfind_com, 1);
    }
    strcpy(root.path, opt->workdir);
    q_push(&root);
  }

  while (1) {
    double now = MPI_Wtime();

    if (opt->stonewall_timer && !runtime.stonewall_hit && now >= runtime.stonewall_endtime) {
      runtime.stonewall_hit = 1;
      q_clear();
      if (opt->verbosity > 1)
        fprintf(runtime.logfile, "Hit stonewall at %.2fs\n", now - start);
    }

    work_t w;
    if (!runtime.stonewall_hit && q_pop(&w))
      process_item(&w);

    serve_steal_requests();
    reap_resp_sends(0);
    progress_steal();
    maybe_steal(MPI_Wtime());

    if (progress_termination(q_count() > 0, MPI_Wtime()))
      break;
    progress_report(0);
  }
  progress_report(1);

  /*
   * Shutdown: finish our own pending steal (victims keep serving until the
   * barrier completes, and nobody enters the barrier with a request in
   * flight), then keep serving until everyone is here.
   */
  while (steal_victim >= 0) {
    serve_steal_requests();
    reap_resp_sends(0);
    progress_steal();
  }
  {
    MPI_Request bar;
    int done = 0;
    CHECK_MPI(MPI_Ibarrier(pfind_com, &bar));
    while (!done) {
      serve_steal_requests();
      reap_resp_sends(0);
      CHECK_MPI(MPI_Test(&bar, &done, MPI_STATUS_IGNORE));
    }
  }
  reap_resp_sends(1);

  if (wd_interval > 0) {
    wd_stop = 1;
    pthread_join(wd_thread, NULL);
  }

  double end = MPI_Wtime();
  res->runtime = end - start;
  res->rate = res->runtime > 0 ? res->total_files / res->runtime : 0;
  res->com = pfind_com;

  if (runtime.logfile != stdout)
    fclose(runtime.logfile);
  free(q);
  q = NULL;
  q_cap = q_head = q_tail = 0;

  MPI_Barrier(pfind_com);
  pfind_dfs_fini(pfind_com);
  return res;
}

pfind_find_results_t *pfind_aggregrate_results(pfind_find_results_t *local){
  pfind_find_results_t *res_tmp = smalloc(sizeof(pfind_find_results_t));
  memcpy(res_tmp, local, sizeof(*res_tmp));

  MPI_Reduce(pfind_rank == 0 ? MPI_IN_PLACE : &res_tmp->errors, &res_tmp->errors, 5,
             MPI_LONG_LONG_INT, MPI_SUM, 0, local->com);
  MPI_Reduce(pfind_rank == 0 ? MPI_IN_PLACE : &res_tmp->runtime, &res_tmp->runtime, 1,
             MPI_DOUBLE, MPI_MAX, 0, local->com);

  res_tmp->rate = res_tmp->runtime > 0 ? res_tmp->total_files / res_tmp->runtime : 0;
  return res_tmp;
}
