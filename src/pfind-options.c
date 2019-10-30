#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include <mpi.h>
#include <regex.h>

#include "pfind-options.h"


// $io500_find_cmd $io500_workdir -newer $timestamp_file -size ${mdt_hard_fsize}c -name *01*

void pfind_abort(char * str){
  printf("%s", str);
  exit(1);
}

static void pfind_print_help(pfind_options_t * res){
  printf("pfind \nSynopsis:\n"
      "pfind <workdir> [-newer <timestamp file>] [-size <size>c] [-name <substr>] [-regex <regex>]\n"
      "\tworkdir = \"%s\"\n"
      "\t-newer = \"%s\"\n"
      "\t-name|-regex = \"%s\"\n"
      "Optional flags\n"
      "\t-N: steal with 50%% likelihood from the next process instead of randomly\n"
      "\t-C: don't output file names just count the number of files found\n"
      "\t-P: output per process for debugging and checks loadbalance\n"
      "\t-D [rates]: print rates\n"
      "\t-s <seconds>: Stonewall timer for find = %d\n"
      "\t-h: prints the help\n"
      "\t--help: prints the help without initializing MPI\n"
      "\t-r <results_dir>: Where to store results = %s\n"
      "\t-v: increase the verbosity, use multiple times to increase level = %d\n"
      "\t-q: queue length (max pending ops) = %d\n",
      res->workdir,
      res->timestamp_file,
      res->name_pattern,
      res->stonewall_timer,
      res->results_dir,
      res->verbosity,
      res->queue_length
    );
}

pfind_options_t * pfind_parse_args(int argc, char ** argv, int force_print_help){
  pfind_options_t * res = malloc(sizeof(pfind_options_t));
  memset(res, 0, sizeof(pfind_options_t));
  int print_help = force_print_help;

  res->workdir = "./";
  res->results_dir = NULL;
  res->verbosity = 0;
  res->timestamp_file = NULL;
  res->name_pattern = NULL;
  res->size = UINT64_MAX;
  res->queue_length = 100000;
  res->max_dirs_per_iter = 1000;
  char * firstarg = NULL;

  #define NONE_STR "-x"

  // when we find special args, we process them
  // but we need to replace them with -x so that getopt will ignore them
  // and getopt will continue to process beyond them
  for(int i=1; i < argc - 1; i++){
    int rc;
    if(strcmp(argv[i], "-newer") == 0){
      res->timestamp_file = strdup(argv[i+1]);
      argv[i] = NONE_STR;
      argv[++i] = NONE_STR;
    }else if (strcmp(argv[i], "-dfs_dir") == 0){
      res->dfs_dir = strdup(argv[i+1]);
      argv[i] = NONE_STR;
      argv[++i] = NONE_STR;
    }else if (strcmp(argv[i], "-pool") == 0) {
      printf("POOL UUID = %s\n", argv[i+1]);
      rc = uuid_parse(argv[i+1], res->pool_uuid);
      if (rc)
          pfind_abort("Invalid Pool UUID\n");
      argv[i] = NONE_STR;
      argv[++i] = NONE_STR;
    }else if (strcmp(argv[i], "-cont") == 0) {
      printf("CONT UUID = %s\n", argv[i+1]);
      rc = uuid_parse(argv[i+1], res->cont_uuid);
      if (rc)
          pfind_abort("Invalid Cont UUID\n");
      argv[i] = NONE_STR;
      argv[++i] = NONE_STR;
    }else if (strcmp(argv[i], "-svcl") == 0) {
      printf("SVCL = %s\n", argv[i+1]);
      res->svcl = strdup(argv[i+1]);
      argv[i] = NONE_STR;
      argv[++i] = NONE_STR;
    }else if(strcmp(argv[i], "-size") == 0){
      char * str = argv[i+1];
      char extension = str[strlen(str)-1];
      str[strlen(str)-1] = 0;
      res->size = atoll(str);
      switch(extension){
        case 'c': break;
        default:
          pfind_abort("Unsupported exension for -size\n");
      }
      argv[i] = NONE_STR;
      argv[++i] = NONE_STR;
    }else if(strcmp(argv[i], "-name") == 0){
      res->name_pattern = malloc(strlen(argv[i+1])*4+100);
      // transform a traditional name pattern to a regex:
      char * str = argv[i+1];
      char * out = res->name_pattern;
      int pos = 0;
      for(unsigned j=0; j < strlen(str); j++){
        if(str[j] == '*'){
          pos += sprintf(out + pos, ".*");
        }else if(str[j] == '.'){
          pos += sprintf(out + pos, "[.]");
        }else if(str[j] == '"' || str[j] == '\"'){
          // erase the "
        }else{
          out[pos] = str[j];
          pos++;
        }
      }
      out[pos] = 0;

      int ret = regcomp(& res->name_regex, res->name_pattern, 0);
      if(ret){
        pfind_abort("Invalid regex for name given\n");
      }
      argv[i] = NONE_STR;
      argv[++i] = NONE_STR;
    }else if(strcmp(argv[i], "-regex") == 0){
      res->name_pattern = strdup(argv[i+1]);
      int ret = regcomp(& res->name_regex, res->name_pattern, 0);
      if(ret){
        pfind_abort("Invalid regex for name given\n");
      }
      argv[i] = NONE_STR;
      argv[++i] = NONE_STR;
    }else if(! firstarg){
      firstarg = strdup(argv[i]);
      argv[i] = NONE_STR;
    }
  }
  if(argc == 2){
    firstarg = strdup(argv[1]);
  }

  int c;
  while ((c = getopt(argc, argv, "CPs:r:vhD:xq:N")) != -1) {
    if (c == -1) {
        break;
    }

    switch (c) {
    case 'N':
      res->steal_from_next = 1;
      break;
    case 'x':
        /* ignore fake arg that we added when we processed the extra args */
        break;
    case 'P':
      res->print_by_process = 1;
      break;
    case 'C':
      res->just_count = 1;
      break;
    case 'D':
      if(strcmp(optarg, "rates") == 0){
        res->print_rates = 1;
      }else{
        pfind_abort("Unsupported debug flag\n");
      }
      break;
    case 'h':
      print_help = 1; break;
    case 'r':
      res->results_dir = strdup(optarg); break;
    case 'q':
      res->queue_length = atoi(optarg); break;
      if(res->queue_length < 10){
        pfind_abort("Queue must be at least 10 elements!\n");
      }
      break;
    case 's':
      res->stonewall_timer = atol(optarg);
      break;
    case 'v':
      res->verbosity++; break;
    case 0:
      break;
    }
  }
  if(res->verbosity > 2 && pfind_rank == 0){
    printf("Regex: %s\n", res->name_pattern);
  }

  if(print_help){
    if(pfind_rank == 0)
      pfind_print_help(res);
    int init;
    MPI_Initialized( & init);
    if(init){
      MPI_Finalize();
    }
    exit(0);
  }

  res->workdir = res->dfs_dir;

  return res;
}
