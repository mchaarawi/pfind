#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <mpi.h>
#include <assert.h>

#include "pfind-options.h"

int pfind_rank;
int pfind_size;
daos_handle_t poh;
daos_handle_t coh;

enum handleType {
        POOL_HANDLE,
        CONT_HANDLE,
	ARRAY_HANDLE
};

/* For DAOS methods. */
#define DCHECK(rc, format, ...)                                         \
do {                                                                    \
        int _rc = (rc);                                                 \
                                                                        \
        if (_rc != 0) {                                                  \
                fprintf(stderr, "ERROR (%s:%d): %d: %d: "               \
                        format"\n", __FILE__, __LINE__, pfind_rank, _rc, \
                        ##__VA_ARGS__);                                 \
                fflush(stderr);                                         \
                exit(-1);                                       	\
        }                                                               \
} while (0)

/* Distribute process 0's pool or container handle to others. */
static void
HandleDistribute(daos_handle_t *handle, enum handleType type)
{
        d_iov_t global;
        int        rc;

        global.iov_buf = NULL;
        global.iov_buf_len = 0;
        global.iov_len = 0;

        assert(type == POOL_HANDLE || type == CONT_HANDLE);
        if (pfind_rank == 0) {
                /* Get the global handle size. */
                if (type == POOL_HANDLE)
                        rc = daos_pool_local2global(*handle, &global);
                else
                        rc = daos_cont_local2global(*handle, &global);
                DCHECK(rc, "Failed to get global handle size");
        }

        MPI_Bcast(&global.iov_buf_len, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
 
	global.iov_len = global.iov_buf_len;
        global.iov_buf = malloc(global.iov_buf_len);
        if (global.iov_buf == NULL)
		MPI_Abort(MPI_COMM_WORLD, -1);

        if (pfind_rank == 0) {
                if (type == POOL_HANDLE)
                        rc = daos_pool_local2global(*handle, &global);
                else
                        rc = daos_cont_local2global(*handle, &global);
                DCHECK(rc, "Failed to create global handle");
        }

        MPI_Bcast(global.iov_buf, global.iov_buf_len, MPI_BYTE, 0, MPI_COMM_WORLD);

        if (pfind_rank != 0) {
                if (type == POOL_HANDLE)
                        rc = daos_pool_global2local(global, handle);
                else
                        rc = daos_cont_global2local(poh, global, handle);
                DCHECK(rc, "Failed to get local handle");
        }

        free(global.iov_buf);
}

static void print_result(pfind_options_t * options, pfind_find_results_t * find, char * prefix){
  if(options->print_rates){
    printf("[%s] rate: %.3f kiops time: %.1fs", prefix, find->rate / 1000, find->runtime);
  }else{
    printf("[%s]", prefix);
  }
  printf(" found: %ld (scanned %ld files, scanned dirents: %ld, unknown dirents: %ld)\n", find->found_files, find->total_files, find->checked_dirents, find->unknown_file);
}

int main(int argc, char ** argv){
  // output help with --help to enable running without mpiexec
  for(int i=0; i < argc; i++){
    if (strcmp(argv[i], "--help") == 0){
      argv[i][0] = 0;
      pfind_rank = 0;
      pfind_parse_args(argc, argv, 1);
      exit(0);
    }
  }

  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, & pfind_rank);
  MPI_Comm_size(MPI_COMM_WORLD, & pfind_size);

  pfind_options_t * options = pfind_parse_args(argc, argv, 0);
  int rc;

  rc = daos_init();
  DCHECK(rc, "Failed to initialize daos");

  if (pfind_rank == 0) {
	  d_rank_list_t *svcl = NULL;
	  daos_pool_info_t pool_info;
	  daos_cont_info_t co_info;

	  svcl = daos_rank_list_parse(options->svcl, ":");
	  if (svcl == NULL)
		  MPI_Abort(MPI_COMM_WORLD, -1);

	  /** Connect to DAOS pool */
	  rc = daos_pool_connect(options->pool_uuid, NULL, svcl, DAOS_PC_RW,
				 &poh, &pool_info, NULL);
	  d_rank_list_free(svcl);
	  DCHECK(rc, "Failed to connect to pool");

	  rc = daos_cont_open(poh, options->cont_uuid, DAOS_COO_RW, &coh, &co_info,
			      NULL);
	  /* If NOEXIST we create it */
	  if (rc)
		  DCHECK(rc, "Failed to open container");
  }
  HandleDistribute(&poh, POOL_HANDLE);
  HandleDistribute(&coh, CONT_HANDLE);

  rc = dfs_mount(poh, coh, O_RDWR, &options->dfs);
  DCHECK(rc, "Failed to mount DFS namespace");

  pfind_find_results_t * find = pfind_find(options);

  if (options->print_by_process){
    char rank[15];
    sprintf(rank, "%d", pfind_rank);
    print_result(options, find, rank);
  }

  pfind_find_results_t * aggregated = pfind_aggregrate_results(find);
  if(pfind_rank == 0){
    print_result(options, aggregated, "DONE");
    printf("MATCHED %ld/%ld\n", aggregated->found_files, aggregated->total_files);
  }

  rc = dfs_umount(options->dfs);
  DCHECK(rc, "Failed to umount DFS namespace");
  MPI_Barrier(MPI_COMM_WORLD);

  rc = daos_cont_close(coh, NULL);
  DCHECK(rc, "Failed to close container %s (%d)", options->cont_uuid, rc);
  MPI_Barrier(MPI_COMM_WORLD);

  rc = daos_pool_disconnect(poh, NULL);
  DCHECK(rc, "Failed to disconnect from pool");
  MPI_Barrier(MPI_COMM_WORLD);

  rc = daos_fini();
  DCHECK(rc, "Failed to finalize DAOS");

  free(find);

  MPI_Finalize();
  return 0;
}
