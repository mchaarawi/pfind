#include <daos.h>
#include <daos_fs.h>


void pfind_init_daos(MPI_Comm comm);
void pfind_fini_daos(MPI_Comm comm);
DIR* pfind_dfs_opendir(const char* dir, int *nr);
int pfind_dfs_closedir(DIR* _dirp);
struct dirent* pfind_dfs_readdir(DIR* _dirp, daos_obj_id_t *oid, daos_size_t *csize, uint64_t *_nr_scanned);
int pfind_dfs_lstat(const char* path, daos_obj_id_t *oid, daos_size_t csize, struct stat* buf);
int pfind_dfs_set_filter(time_t ctime_min, const char *name);
