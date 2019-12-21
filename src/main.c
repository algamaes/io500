#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <mpi.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <io500-util.h>
#include <io500-phase.h>

static char const * io500_phase_str[IO500_SCORE_LAST] = {
  "NO SCORE",
  "MD",
  "BW"};

static u_phase_t * phases[IO500_PHASES] = {
  & p_opt,
  & p_debug,

  & p_ior_easy,
  & p_ior_easy_write,

  & p_mdtest_easy,
  & p_mdtest_easy_write,
  & p_timestamp,

  & p_ior_hard,
  & p_ior_hard_write,

  & p_mdtest_hard,
  & p_mdtest_hard_write,

  & p_find,

  & p_ior_easy_read,
  & p_mdtest_easy_stat,

  & p_ior_hard_read,
  & p_mdtest_hard_stat,

  & p_mdtest_easy_delete,
  & p_mdtest_hard_read,
  & p_mdtest_hard_delete
};

static ini_section_t ** options(void){
  ini_section_t ** ini_section = u_malloc(sizeof(ini_section_t*) * (IO500_PHASES + 1));
  for(int i=0; i < IO500_PHASES; i++){
    ini_section[i] = u_malloc(sizeof(ini_section_t));
    ini_section[i]->name = phases[i]->name;
    ini_section[i]->option = phases[i]->options;
  }
  ini_section[IO500_PHASES] = NULL;
  return ini_section;
}

static void parse_ini_file(char * file, ini_section_t** cfg){
  struct stat statbuf;
  int ret = stat(file, & statbuf);
  if(ret != 0){
    FATAL("Cannot open config file %s\n", file);
  }

  char * buff = "";
  if(statbuf.st_size > 0){
    buff = malloc(statbuf.st_size + 1);
    if(! buff){
      FATAL("Cannot malloc();")
    }

    FILE * f = fopen(file, "r");
    if(ret != 0){
      FATAL("Cannot open config file %s\n", file);
    }
    ret = fread(buff, statbuf.st_size, 1, f);
    fclose(f);
    if( ret != 1 ){
      FATAL("Couldn't read config file %s\n", file);
    }
    buff[statbuf.st_size] = '\0';
  }

  ret = u_parse_ini(buff, cfg);
  if (ret != 0){
    FATAL("Couldn't parse config file %s\n", file);
  }

  free(buff);
}

static void init_result_dir(void){
  int ret;
  char buffer[30];

  if(opt.rank == 0){
    struct tm* tm_info;
    time_t timer;
    time(&timer);
    tm_info = localtime(&timer);
    strftime(buffer, 30, "%Y.%m.%d-%H.%M.%S", tm_info);

    ret = mkdir("results", S_IRWXU);
    if(ret != 0 && errno != EEXIST){
      FATAL("Couldn't create directory: \"results\" (Error: %s)\n", strerror(errno));
    }
  }
  MPI_Bcast(buffer, 30, MPI_CHAR, 0, MPI_COMM_WORLD);

  char resdir[2048];
  sprintf(resdir, "./results/%s", buffer);
  if(opt.rank == 0){
    PRINT_PAIR("result-dir", "%s\n", resdir);
    ret = mkdir(resdir, S_IRWXU);
    if(ret != 0){
      FATAL("Couldn't create directory %s (Error: %s)\n", resdir, strerror(errno));
    }
  }
  opt.resdir = strdup(resdir);

  sprintf(resdir, "%s/%s", opt.datadir, buffer);
  opt.datadir = strdup(resdir);
}

int main(int argc, char ** argv){
  ini_section_t ** cfg = options();

  MPI_Init(& argc, & argv);
  MPI_Comm_rank(MPI_COMM_WORLD, & opt.rank);
  MPI_Comm_size(MPI_COMM_WORLD, & opt.mpi_size);

  if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0){
    help:
    if(opt.rank != 0){
      exit(0);
    }
    r0printf("Synopsis: %s <INI file> [-v=<verbosity level>] [--dry-run]\n\n", argv[0]);
    r0printf("Supported and current values of the ini file:\n");
    u_ini_print_values(cfg);
    exit(1);
  }

  int print_help = 0;
  if(argc > 2){
    for(int i = 2; i < argc; i++){
      if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ){
        print_help = 1;
      }else if(strncmp(argv[i], "-v=", 3) == 0){
        opt.verbosity = atoi(argv[i]+3);
      }else if(strcmp(argv[i], "--dry-run") == 0 ){
        opt.dry_run = 1;
      }else{
        FATAL("Unknown option: %s\n", argv[i]);
      }
    }
  }

  parse_ini_file(argv[1], cfg);
  if(print_help){
    goto help;
  }

  init_result_dir();

  if(opt.rank == 0){
    PRINT_PAIR_HEADER("config-hash");
    u_ini_print_hash(stdout, cfg);
    printf("\n");    
  }

  MPI_Barrier(MPI_COMM_WORLD);
  if(opt.verbosity > 0 && opt.rank == 0){
    printf("; START ");
    u_print_timestamp();
    printf("\n");
  }

  for(int i=0; i < IO500_PHASES; i++){
    phases[i]->validate();
  }
  if(opt.rank == 0){
    printf("\n");
  }



  for(int i=0; i < IO500_PHASES; i++){
    if(! phases[i]->run) continue;
    MPI_Barrier(MPI_COMM_WORLD);
    if(opt.rank == 0){
      printf("\n[%s]\n", phases[i]->name);
      if(opt.verbosity > 0){
        PRINT_PAIR_HEADER("t_start");
        u_print_timestamp();
        printf("\n");
      }
    }

    double start = GetTimeStamp();
    double score = phases[i]->run();
    if(opt.rank == 0 && phases[i]->group > IO500_NO_SCORE){
      PRINT_PAIR("score", "%f\n", score);
    }
    phases[i]->score = score;

    double runtime = GetTimeStamp() - start;
    // This is an additional sanity check
    if( phases[i]->verify_stonewall && opt.rank == 0){
      if(runtime < opt.stonewall && ! opt.dry_run){
        opt.is_valid_run = 0;
        ERROR("Runtime of phase (%f) is below stonewall time. This shouldn't happen!\n", runtime);
      }
    }

    if(opt.verbosity > 0 && opt.rank == 0){
      PRINT_PAIR("t_delta", "%.4f\n", runtime);
      PRINT_PAIR_HEADER("t_end");
      u_print_timestamp();
      printf("\n");
    }
  }

  MPI_Barrier(MPI_COMM_WORLD);
  if(opt.rank == 0){
    if(opt.verbosity > 0){
      printf("; END ");
      u_print_timestamp();
      printf("\n");
    }

    // compute the overall score
    printf("\n[SCORE]\n");
    double overall_score = 0;

    for(int g=1; g < IO500_SCORE_LAST; g++){
      char score_string[2048];
      char *p = score_string;
      double score = 0;
      int numbers = 0;
      p += sprintf(p, " %s = (", io500_phase_str[g]);
      for(int i=0; i < IO500_PHASES; i++){
        if(phases[i]->group == g){
          double t = phases[i]->score;
          score += t*t;
          if(numbers > 0)
            p += sprintf(p, " + ");
          numbers++;
          p += sprintf(p, "(%.3f*%.3f)", t, t);
        }
      }
      DEBUG_INFO("%s)^%f\n", score_string, 1.0/numbers);
      score = pow(score, 1.0/numbers);
      PRINT_PAIR(io500_phase_str[g], "%.3f\n", score);
      overall_score += score * score;
    }
    PRINT_PAIR("SCORE", "%.3f %s\n", sqrt(overall_score), opt.is_valid_run ? "" : " [INVALID]");
  }
  MPI_Finalize();
  return 0;
}
