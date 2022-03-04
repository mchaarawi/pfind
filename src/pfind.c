#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <mpi.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#ifdef LZ4
#include <lz4.h>
#endif

#include "pfind-options.h"
#include "pfind-dfs.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#warning "Could not detect PATH_MAX"
#endif

#define MSG_COMPLETE 777
#define MSG_JOB_STEAL 800
#define MSG_JOB_STEAL_RESPONSE 820

//#define debug printf
#define debug(...)

// this version is based on mpistat
// parallel recursive find

// globals
static char start_dir[PATH_MAX]; // absolute path of start directory
static int start_dir_length;
static int msg_type_flag = 0;

static pfind_options_t * opt;
static pfind_find_results_t * res = NULL;

static unsigned long readdirs=0, lstats=0;
static double readdirs_agg_time=0, lstats_agg_time=0;

#define DIR_END_OFFSET (0x3fffffffffffffff)
#define MAX_DIR_OFFSET ((DIR_END_OFFSET+1) | 0xffffffffUL)
#define MAX_DIR_FRAGMENTS (32)

unsigned int reverse(unsigned int x, int bits)
{
	x = ((x & 0x55555555) << 1) | ((x & 0xAAAAAAAA) >> 1); // Swap _<>_
	x = ((x & 0x33333333) << 2) | ((x & 0xCCCCCCCC) >> 2); // Swap __<>__
	x = ((x & 0x0F0F0F0F) << 4) | ((x & 0xF0F0F0F0) >> 4); // Swap ____<>____
	x = ((x & 0x00FF00FF) << 8) | ((x & 0xFF00FF00) >> 8); // Swap ...
	x = ((x & 0x0000FFFF) << 16) | ((x & 0xFFFF0000) >> 16); // Swap ...
	return x >> (32 - bits);
}

unsigned int to_serial(long d_off) {
	return (d_off & (DIR_END_OFFSET+1)) ? reverse(d_off, 32) : (d_off >> 32);
}

off_t from_serial(unsigned int serial) {
	return (DIR_END_OFFSET+1) | (unsigned long)reverse(serial, 32);
}

typedef struct{
	uint64_t    ctime_min;
	double      stonewall_endtime;
	FILE *      logfile;
	int         needs_stat;
} pfind_runtime_options_t;

static pfind_runtime_options_t runtime;
static void find_do_readdir(char *path, int idx);
static void find_do_lstat(char *path);

typedef struct {
	char type; // 'd' for directory
	off_t next;
	off_t end;
	char name[PATH_MAX];
} work_t;

static DIR * open_dir = NULL;
static char open_dir_name[PATH_MAX];

// queue of work
static work_t * work;
static int head = 0, tail = 0;
// amount of pending_work
static int pending_work() {
	int size = opt->queue_length+1;
	return (head >= tail) ? (head - tail) : (head + size - tail);
}
static int work_queue_empty() { return head == tail; }
static int work_queue_full() { return pending_work() >= opt->queue_length; }
static void discard_all() {
	head = 0;
	tail = 0;
}

static int items_processed = 0;
static int stole = 0;

static void* smalloc(size_t size){
	void * p = malloc(size);
	if(p == NULL){
		printf("Cannot allocate %zd bytes of memory\nAborting\n", size);
		exit(1);
	}
	return p;
}

static int enqueue_work_imp(work_t *slot, char typ, char * path, char * entry){
	int ret = work_queue_full();
	if (ret){
		return 1;
	}

	slot->type = typ;
	if (entry) {
		sprintf(slot->name, "%s/%s", path, entry);
	} else {
		strcpy(slot->name, path);
	}
	//printf("Queuing: %s\n", work[head].name);
	//fprintf(runtime.logfile, "Queuing: %s %lx-%lx\n", slot->name, slot->next, slot->end);
	return 0;
}

static int enqueue_work(char typ, char * path, char * entry){
	int ret = enqueue_work_imp(&work[head], typ, path, entry);
	if (ret){
		return 1;
	}
	head++;
	if (head > opt->queue_length) { head = 0; }
	return 0;
}

static int tail_enqueue_work(char typ, char * path, char * entry){
	int new_tail = (tail == 0) ? opt->queue_length : (tail-1);
	int ret = enqueue_work_imp(&work[new_tail], typ, path, entry);
	if (ret){
		return 1;
	}
	tail = new_tail;
	return 0;
}

static void find_process_one_item(){
	if(open_dir){
		fprintf(runtime.logfile, "error: %s still open\n", open_dir_name);
		return;
	}
	work_t * cur = & work[tail];
	tail++;
	if (tail > opt->queue_length) { tail = 0; }
	items_processed++;
	//fprintf(runtime.logfile, "Dequeuing: %s of type %c, %lx-%lx, %d pending\n", cur->name, cur->type, cur->next, cur->end, pending_work());

	int idx;
	char tmp_path[PATH_MAX];

	if (strncmp(cur->name, "~~", 2) == 0) { 
		sscanf(cur->name, "~~%d:%s", &idx, tmp_path);
	} else {
		strcpy(tmp_path, cur->name);
		idx = -1;
	}

	if( cur->type == 'd'){
		find_do_readdir(tmp_path, idx);
	}
}

#define CHECK_MPI if(ret != MPI_SUCCESS){ printf("ERROR in %d\n", __LINE__); exit (1); }

pfind_find_results_t * pfind_find(pfind_options_t * lopt){
	opt = lopt;
	memset(& runtime, 0, sizeof(pfind_runtime_options_t));
	int ret;

	pfind_init_daos();

	if(opt->timestamp_file){
		if(pfind_rank==0) {
			static struct stat timer_file;
			if(lstat(opt->timestamp_file, & timer_file) != 0) {
				pfind_abort("Could not read timestamp file!");
			}
			runtime.ctime_min = timer_file.st_ctime;
		}
		MPI_Bcast(&runtime.ctime_min, 1, MPI_INT, 0, MPI_COMM_WORLD);
	}

	if(opt->results_dir && ! opt->just_count){
		char outfile[10240];
		sprintf(outfile, "%s/%d.txt", opt->results_dir, pfind_rank);
		mkdir(opt->results_dir, S_IRWXU);
		runtime.logfile = fopen(outfile, "w");

		if(runtime.logfile == NULL){
			pfind_abort("Could not open output logfile!\n");
		}
	}else{
		runtime.logfile = stdout;
	}

	// allocate sufficient large buffer size, we may receive up to size unexpected "stealing" calls
	int bsend_size = (pfind_size + 1) * MPI_BSEND_OVERHEAD;
	char * bsend_buf = smalloc( bsend_size );
	MPI_Buffer_attach( bsend_buf, bsend_size );

	if(opt->timestamp_file || opt->size != UINT64_MAX){
		runtime.needs_stat = 1;
	}
	//ior_aiori_t * backend = aiori_select(opt->backend_name);
	work = smalloc(sizeof(work_t) * (opt->queue_length + 1 ));
	char * send_buf = smalloc(sizeof(work_t) * (opt->queue_length + 1 ) / 2);
	double start = MPI_Wtime();
	strcpy(start_dir, opt->workdir);
	res = smalloc(sizeof(pfind_find_results_t));
	memset(res, 0, sizeof(*res));

#ifdef LZ4
	int max_compressed = LZ4_COMPRESSBOUND(opt->queue_length * sizeof(work_t) / 2);
	char * compress_buf = smalloc(max_compressed);
#endif

	if(opt->stonewall_timer != 0){
		runtime.stonewall_endtime = MPI_Wtime() + opt->stonewall_timer;
	}else{
		runtime.stonewall_endtime = 0;
	}

	start_dir_length = strlen(start_dir);

	// startup, load current directory
	if(pfind_rank == 0){
		enqueue_work('d', start_dir, NULL);
	}

	int have_finalize_token = (pfind_rank == 0);
	// a token indicating that I'm msg_type and other's to the left
	int have_processed_work_after_token = 1;
	int phase = 0; // 1 == potential cleanup; 2 == stop query; 3 == terminate

	while(! msg_type_flag){
		//usleep(100000);
		debug("[%d] processing: %d [%d, %d, phase: %d]\n", pfind_rank, pending_work(), have_finalize_token, have_processed_work_after_token, phase);
		// do we have more work?
		if(!work_queue_empty()){
			if (opt->stonewall_timer && MPI_Wtime() >= runtime.stonewall_endtime ){
				if(opt->verbosity > 1){
					printf("Hit stonewall at %.2fs\n", MPI_Wtime());
				}
				discard_all();
			}else{
				find_process_one_item();
			}
		}

		int has_msg;
		// try to retrieve token from a neighboring process
		int left_neighbor = pfind_rank == 0 ? pfind_size - 1 : (pfind_rank - 1);
		ret = MPI_Iprobe(left_neighbor, MSG_COMPLETE, MPI_COMM_WORLD, & has_msg, MPI_STATUS_IGNORE);
    CHECK_MPI
	    if(has_msg){
		    ret = MPI_Recv(& phase, 1, MPI_INT, left_neighbor, MSG_COMPLETE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      CHECK_MPI
	      have_finalize_token = 1;
      debug("[%d] recvd finalize token\n", pfind_rank);
	    }

    // we msg_type our last piece of work
    if (work_queue_empty()){
	    // Exit condition requesting_rank
	    if (have_finalize_token){
		    if (have_processed_work_after_token){
			    phase = 0;
		    }else if(pfind_rank == 0){
			    phase++;
		    }
		    if( pfind_size == 1){
			    break;
		    }
		    // send the finalize token to the right process
		    debug("[%d] forwarding token\n", pfind_rank);
		    ret = MPI_Bsend(& phase, 1, MPI_INT, (pfind_rank + 1) % pfind_size, MSG_COMPLETE, MPI_COMM_WORLD);
        CHECK_MPI
		// we received the finalization token
		if (phase == 3){
			break;
		}
        have_processed_work_after_token = 0;
        have_finalize_token = 0;
	    }
    }

    MPI_Status wait_status;
    // check for job-stealing request
    has_msg = 1;
    while(has_msg){
	    ret = MPI_Iprobe(MPI_ANY_SOURCE, MSG_JOB_STEAL, MPI_COMM_WORLD, & has_msg, & wait_status);
      CHECK_MPI
	      if(has_msg){
		      int requesting_rank = wait_status.MPI_SOURCE;
		      ret = MPI_Recv(NULL, 0, MPI_INT, requesting_rank, MSG_JOB_STEAL, MPI_COMM_WORLD, & wait_status);
        CHECK_MPI
		debug("[%d] msg ready from %d !\n", pfind_rank, requesting_rank);
        int distance = (pfind_rank + pfind_size - requesting_rank) % pfind_size;
        int symmetric_dist = (distance <= pfind_size/2) ? distance : (pfind_size - distance);
        int work_to_give = (pending_work() * symmetric_dist) / pfind_size;
        int copied = 0;
        int datasize = work_to_give * sizeof(work_t);
        if (tail + work_to_give > opt->queue_length) {
		copied = (opt->queue_length+1-tail) * sizeof(work_t);
		memcpy(send_buf, & work[tail], copied);
		work_to_give -= (opt->queue_length+1-tail);
		tail = 0;
        }
        memcpy(send_buf + copied, & work[tail], datasize-copied);
        tail += work_to_give;
        if (tail > opt->queue_length) { tail -= opt->queue_length+1; }

        if (work_to_give) {
		//fprintf(runtime.logfile, "%.2f: sending %d entries to %d\n", MPI_Wtime(), work_to_give, requesting_rank);
        }
        #ifndef LZ4
	ret = MPI_Send(send_buf, datasize, MPI_CHAR, requesting_rank, MSG_JOB_STEAL_RESPONSE, MPI_COMM_WORLD);
        #else
        if(datasize > 0){
		int compressed_size;
		compressed_size = LZ4_compress_default(send_buf, compress_buf, datasize, max_compressed);
		ret = MPI_Send(compress_buf, compressed_size, MPI_CHAR, requesting_rank, MSG_JOB_STEAL_RESPONSE, MPI_COMM_WORLD);
        }else{
		ret = MPI_Send(NULL, 0, MPI_CHAR, requesting_rank, MSG_JOB_STEAL_RESPONSE, MPI_COMM_WORLD);
        }
        #endif
        CHECK_MPI
		}
    }

    int work_received = 0;
    int steal_neighbor = pfind_rank;
    if (work_queue_empty() && phase < 2){
	    // send request for job stealing
	    if(opt->steal_from_next) {
		    steal_neighbor = ((rand() % 2) && (pfind_rank > 0)) ? pfind_rank - 1 : rand() % pfind_size;
	    }else{
		    int range = 2 + rand() % (pfind_size-1); // adding 2 so we never pick a trivial range
		    int sign = (range%2) * 2 -1;
		    steal_neighbor = (pfind_rank + pfind_size + sign*(range/2)) % pfind_size;
	    }
	    if(steal_neighbor != pfind_rank){
		    //fprintf(runtime.logfile, "%.2f: [%d] msg attempting to steal from %d\n", MPI_Wtime(), pfind_rank, steal_neighbor);
		    ret = MPI_Bsend(NULL, 0, MPI_INT, steal_neighbor, MSG_JOB_STEAL, MPI_COMM_WORLD);
        CHECK_MPI
		// check for any pending work stealing request, too.
		while(1){
			ret = MPI_Iprobe(steal_neighbor, MSG_JOB_STEAL_RESPONSE, MPI_COMM_WORLD, & has_msg, & wait_status);
          CHECK_MPI
		  if(has_msg){
			  MPI_Get_count(& wait_status, MPI_CHAR, & work_received);

            #ifndef LZ4
			  ret = MPI_Recv(work, work_received, MPI_CHAR, steal_neighbor, MSG_JOB_STEAL_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            #else
			  if(work_received > 0){
				  ret = MPI_Recv(compress_buf, work_received, MPI_CHAR, steal_neighbor, MSG_JOB_STEAL_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				  int decompressed_size = LZ4_decompress_safe (compress_buf, (char*)work, work_received, max_compressed);
				  //printf("Before: %d after: %d\n", datasize, compressed_size);
				  work_received = decompressed_size;
			  }else{
				  ret = MPI_Recv(work, work_received, MPI_CHAR, steal_neighbor, MSG_JOB_STEAL_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			  }
            #endif
            CHECK_MPI

		    work_received /= sizeof(work_t);
            break;
		  }
          ret = MPI_Iprobe(MPI_ANY_SOURCE, MSG_JOB_STEAL, MPI_COMM_WORLD, & has_msg, MPI_STATUS_IGNORE);
          CHECK_MPI
		  if(has_msg){
			  ret = MPI_Recv(NULL, 0, MPI_INT, MPI_ANY_SOURCE, MSG_JOB_STEAL, MPI_COMM_WORLD, & wait_status);
            CHECK_MPI
		    int requesting_rank = wait_status.MPI_SOURCE;
            debug("[%d] msg ready from %d, but no work pending!\n", pfind_rank, requesting_rank);
            ret = MPI_Send(NULL, 0, MPI_CHAR, requesting_rank, MSG_JOB_STEAL_RESPONSE, MPI_COMM_WORLD);
            CHECK_MPI
		    }
		}
        stole += work_received;
        //if (work_received) {
		//fprintf(runtime.logfile, "[%d] stole %d from %d\n", pfind_rank, work_received, steal_neighbor);
        //}
        discard_all();
        head = work_received;
        if (work_received > 0){
		have_processed_work_after_token = 1;
        }
	    }
    }
	}
	debug("[%d] ended\n", pfind_rank);

  #ifdef LZ4
	free(compress_buf);
  #endif

	free(send_buf);
	free(work);
	MPI_Buffer_detach(bsend_buf, & bsend_size);

	double end = MPI_Wtime();
	res->runtime = end - start;
	res->rate = res->total_files / res->runtime;

	fprintf(runtime.logfile, "[%d] rate: %.3f kiops time: %.1fs", pfind_rank, res->rate / 1000, res->runtime);
	fprintf(runtime.logfile, " found: %ld (scanned %ld files, scanned dirents: %ld, unknown dirents: %ld)\n", res->found_files, res->total_files, res->checked_dirents, res->unknown_file);
	fprintf(runtime.logfile, " %ld lstats %.6fs each %ld readdirs %.6fs each %d items dequeued (%d stolen)\n", lstats, lstats_agg_time/lstats, readdirs, readdirs_agg_time/readdirs, items_processed, stole);

	if(runtime.logfile != stdout){
		fclose(runtime.logfile);
	}

	pfind_fini_daos();
	return res;
}

pfind_find_results_t * pfind_aggregrate_results(pfind_find_results_t * local){
	pfind_find_results_t * res = smalloc(sizeof(pfind_find_results_t));
	if(! res){
		pfind_abort("No memory");
	}
	memcpy(res, local, sizeof(*res));

	MPI_Reduce(pfind_rank == 0 ? MPI_IN_PLACE : & res->errors, & res->errors, 5, MPI_LONG_LONG_INT, MPI_SUM, 0, MPI_COMM_WORLD);
	MPI_Reduce(pfind_rank == 0 ? MPI_IN_PLACE : & res->runtime, & res->runtime, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

	res->rate = res->total_files / res->runtime;
	return res;
}

static char  find_file_type(unsigned char c) {
	switch (c) {
        case DT_BLK :
		return 'b';
        case DT_CHR :
		return 'c';
        case DT_DIR :
		return 'd';
        case DT_FIFO :
		return 'F';
        case DT_LNK :
		return 'l';
        case DT_REG :
		return 'f';
        case DT_SOCK :
		return 's';
        default :
		return 'u';
	}
}

static void check_buf(struct stat buf, char * path){
	// compare values
	if(opt->timestamp_file){
		if( (uint64_t) buf.st_ctime < runtime.ctime_min ){
			if(opt->verbosity >= 2){
				printf("Timestamp too small: %s\n", path);
			}
			return;
		}
	}
	if(opt->size != UINT64_MAX){
		uint64_t size = (uint64_t) buf.st_size;
		if(size != opt->size){
			if(opt->verbosity >= 2){
				printf("Size does not match: %s has %zu bytes\n", path, (size_t) buf.st_size);
			}
			return;
		}
	}
	if(0 && ! opt->just_count){
		fprintf(runtime.logfile, "%s\n", path);
	}
	res->found_files++;
}

static void find_do_lstat(char *path) {
	struct stat buf;

	// filename comparison has been done already
	if(opt->verbosity >= 2){
		printf("STAT: %s\n", path);
	}

	res->total_files++;

	if (pfind_dfs_lstat(path, &buf) == 0) {
		check_buf(buf, path);
	} else {
		res->errors++;
		fprintf(runtime.logfile, "Error stating file: %s\n", path);
	}
}

static void find_do_readdir(char *path, int idx) {
	DIR *d;

	if (idx == -1) {
		int nr = -1, i;
		char tmp[PATH_MAX];
		
		d = pfind_dfs_opendir(path, &nr);
		for (i=0 ; i<nr; i++) {
			sprintf(tmp, "~~%d:%s", i, path);
			//printf("enqueue %s\n", tmp);
			enqueue_work('d', tmp, NULL);
		}
		pfind_dfs_closedir(d);
		return;
	}

	d = pfind_dfs_opendir(path, &idx);
	if (!d) {
		if(opt->verbosity > 1){
			fprintf(runtime.logfile, "Cannot open '%s': %s\n", path, strerror (errno));
		}
		res->errors++;
		return;
	}

	int processed = 0;

	while (1) {
		//printf("find_do_readdir %s\n", path);

		struct dirent *entry;
		entry = pfind_dfs_readdir(d);
		if (opt->stonewall_timer && MPI_Wtime() >= runtime.stonewall_endtime ){
			if(opt->verbosity > 1){
				fprintf(runtime.logfile, "Hit stonewall at %.2fs\n", MPI_Wtime());
			}
			break;
		}

		if (entry == NULL) {
			break;
		}

		char newpath[PATH_MAX];

		strcpy(newpath, path);
		strcat(newpath, "/");
		strcat(newpath, entry->d_name);

		//debug("[%d] %s\n", pfind_rank, entry->d_name);
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		res->checked_dirents++;
		processed++;

		char typ = find_file_type(entry->d_type);
		if (typ == 'u'){
			res->unknown_file++;
			// sometimes the filetype is not provided by readdir.
			struct stat buf;
			if (pfind_dfs_lstat(newpath, &buf)) {
				res->errors++;
				if(opt->verbosity > 1){
					fprintf(runtime.logfile, "Error stating file: %s\n", newpath);
				}
				continue;
			}
			typ = S_ISDIR(buf.st_mode) ? 'd' : 'f';

			if(typ == 'f') { // since we have done the stat already, it would be a waste to do it again
				res->total_files++;
				// compare values
				if(opt->name_pattern && regexec(& opt->name_regex, entry->d_name, 0, NULL, 0) ){
					if(opt->verbosity >= 2) {
						printf("Name does not match: %s\n", entry->d_name);
					}
					continue;
				}
				check_buf(buf, entry->d_name);
				continue;
			}
		} else if (typ != 'd') {
			assert(0);
			// compare file name
			if(opt->name_pattern && regexec(& opt->name_regex, entry->d_name, 0, NULL, 0) ){
				res->total_files++;
				if(opt->verbosity >= 2){
					printf("Name does not match: %s\n", entry->d_name);
				}
				continue;
			}
			if(! runtime.needs_stat){
				res->total_files++;
				// optimization to skip stat
				res->found_files++;
				if(! opt->just_count){
					fprintf(runtime.logfile, "%s/%s\n", path, entry->d_name);
				}
				continue;
			}
		}

		// we need to perform a stat operation
		if(typ == 'd') {
			enqueue_work(typ, newpath, NULL);
		} else {
			//char cur_path[PATH_MAX];
			//sprintf(cur_path, "%s/%s", path, entry->d_name);
			find_do_lstat(newpath);
		}
	}
	pfind_dfs_closedir(d);
	open_dir = NULL;
}
