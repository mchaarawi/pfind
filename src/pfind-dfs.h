
void pfind_init_daos(MPI_Comm comm);
void pfind_fini_daos(MPI_Comm comm);
DIR* pfind_dfs_opendir(const char* dir, int *nr);
int pfind_dfs_closedir(DIR* _dirp);
struct dirent* pfind_dfs_readdir(DIR* _dirp, uint64_t *_nr_scanned);
int pfind_dfs_lstat(const char* path, struct stat* buf);
int pfind_dfs_set_filter(time_t ctime_min, const char *name);
