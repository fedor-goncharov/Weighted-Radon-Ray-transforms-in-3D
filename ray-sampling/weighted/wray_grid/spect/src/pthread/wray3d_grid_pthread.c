#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <assert.h>
#include <pthread.h>  //parallelization via posix threads
#include <sys/time.h> //measure physical time of evaluation
#include "init.h"
#include "threadfunc.h"

#define FILENAME_MAXSIZE 64
/*
 * The program reads the grid data for the test-function and attenuation map from files 
 * (-if [test-function filename], -ia [attenuation filename]) and computes its ray transforms in slices 
 * of planes (parallel to XY). The set of rays  in each slice plane corresponds to 'uniform geometry' 
 * (uniform shifts x uniform angles on circle). SPECT weight is computed parallely for each point of the grid
 * (for better memory usage (and worse computation time)).
 * The parameters of the input test function an parameters of ray transforms are given in a 
 * separate configuration file (-p filename).
 * 
 * IMPORTANT : It is always assumed that the test-function is compactly supported with support in the
 * centered unit ball (otherwise the generated data will be incomplete and this will produce artifacts in reconstructions)
 *
 * The results of computations are stored in a separate file (-o filename). 
 * 
 * Output is stored in a CSV file.
*/

/* ***************************************************************************
 * ****************************** HANDLING FILES *****************************
 * ***************************************************************************
 */
void data_generate_readme(char* output_filename, char* input_func_filename, char* input_att_filename, int nz, int nshift, int nphi) {
  
  FILE *file;
  char readme_filename [ FILENAME_MAXSIZE ];
  
  sprintf(readme_filename, "rdme_%s", output_filename);

  file = fopen(readme_filename, "w");
  fprintf(file, "  Ray transforms in 3D for test-function in %s, attenuation map in %s\n", input_func_filename, input_att_filename);
  fprintf(file, "  Format of the data: [z], [phi], [shift], [Pf]\n");
  fprintf(file, "  Parameters of the data:\n");
  fprintf(file, "    number of layers OZ direction  [nz]   : %d,\n", nz);
  fprintf(file, "    number of angles on circle     [phi]  : %d,\n", nphi);
  fprintf(file, "    number of shifts per direciton [s]    : %d.\n", nshift);
    
  fclose(file);
}

void data_aggregate_chunks(char* output_filename, char** chunks_filenames, int nchunks) {
  
  FILE* out; //output file
  out = fopen(output_filename, "w");
  
  int i_file;
  for (i_file = 0; i_file < nchunks; ++i_file) {
    //write data from local files (*fp) to output file (*out)
    FILE* fp;
    fp = fopen(chunks_filenames[i_file], "r");
    
    //transmit by chunks of 1 mb
    char buffer[1024 * 1024];
    int size;
    do {
      size = fread(buffer, 1, sizeof(buffer), fp);
      if (size <= 0) break;
      fwrite(buffer, 1, size, out);
    } while (size == sizeof(buffer));
    //reached EOF, close local file
    
    fclose(fp);
    //printf("  Data from %s has been sucessfully transmitted to %s\n", chunks_filenames[i_file], output_filename);
 }
}

/* ***************************************************************************
 * ****************************** HANDLING INPUT *****************************
 * ***************************************************************************
 */


const char* program_name;

/* Prints usage information to STREAM (stdout or stderr), and 
 * exits the program with EXIT_CODE. Does not return. */

void print_usage(FILE* stream, int exit_code) {
  fprintf(stream, "Usage: %s -p (filename) -f (filename) -a (filename) -o (filename) -n (number)\n", program_name);
  fprintf(stream, 
	  "   -h --help                  Display usage information.\n"
	  "   -p --parameters            Read parameters of the test-function and 3D ray transforms from file.\n"
	  "   -f --input_test_function   Read test-functiom data from file (according to configuration parameters in '-p').\n"
	  "   -a --input_attributes      Read attenuation map from file (according to configuration parameters in '-p').\n"
	  "   -o --output                Place output data to a file.\n"
	  "   -n --nthreads              Number of OpenMP threads for parallelization of calculations.\n");
  exit(exit_code);
}

/* ***************************************************************************
 * ****************************** ENTRY POINT ********************************
 * ***************************************************************************
 */

int main(int argc, char * argv[]) {
  
  if (argc == 1) {
     fprintf(stderr, "%s: arguments required. Try '-h' or '--help'.\n", argv[0]);
     exit(EXIT_FAILURE);
  }
  
  int next_option;
  const char* short_options = "hp:f:a:o:n:";
  const struct option long_options[] = {
    {"help", no_argument, 0, 0}, 
    {"parameters", required_argument, 0, 0},
    {"input_test_function", required_argument, 0, 0}, 
    {"input_attributes", required_argument, 0, 0},
    {"output", required_argument, 0, 0},
    {"nthreads", required_argument, 0, 0},
    {NULL, 0, NULL, 0}				/* Required at the end of array */
  };

  char *parameters_filename,
       *input_func_filename,
       *input_att_filename,
       *output_filename;
  int nthreads = 1;	// number of openmp threads
  
  program_name = argv[0];

  /* reading long options */ 
  do {
    next_option = getopt_long(argc, argv, short_options, long_options, NULL);
    
    switch (next_option) {
      case 'h':
	print_usage(stdout, 0);
      case 'p':
	parameters_filename = optarg;
	assert(optarg[0] != '-');
	printf("Parameters file: %s\n", optarg);
	break;
      case 'f':
	input_func_filename = optarg;
	assert(optarg[0] != '-');
	printf("Input file with test-function data: %s\n", optarg);
	break;
      case 'a':
	input_att_filename = optarg;
	assert(optarg[0] != '-');
	printf("Input file with attenuation map data: %s\n", optarg);
	break;
      case 'o':
	output_filename = optarg;
	assert(optarg[0] != '-');
	printf("Output file with ray data: %s\n", optarg);
	break;
      case 'n':
	nthreads = atoi(optarg);
	assert(nthreads > 0);
	printf("Number of threads: %d\n", nthreads);
	break;
      case ':': /* missing argument */ 
	fprintf(stderr, "%s: option '-%c' requires an argument\n",
		argv[0], optopt);
	print_usage(stderr, 1);
      case '?':
	print_usage(stderr, 1);
      case -1: /* Done with the options. */
	break;
      default:
	abort();
    }
  } while (next_option != -1);
  
  if (optind < argc) {
    printf("Warning: non-option ARGV-arguments are omitted. See '-h' or '--help'.\n"); 
  }
  
  /**************************************** START INITIALIZATIONS *****************************************/
  /* Init configuration parameters of the grid and of Radon data from file */
  printf("  Reading parameters from %s...", parameters_filename);
  int *parameters;
  if (parameters_alloc(&parameters) != 0) {
     perror("Aborting : init_parameters_alloc.\n");
     exit(EXIT_FAILURE);
  }
  
  if (parameters_init(parameters_filename, parameters) != 0) {
    perror("Aborting : init_parameters.\n");
    exit(EXIT_FAILURE);
  }
  printf("Done.\n");
  
  int ngrid       = parameters[0];	// number of points per dimension (in [-1,1^3]) for the test-function and attenuation map
  int nshift      = parameters[1];  // number of shifts for the set of rays in a plane
  int nphi        = parameters[2];  // number of angles for the set of rays in a plane
  int nslice      = parameters[3];	// number of z-slices
  
  
  /* Init values for the test function on the grid */
  printf("  Initializing test-function data...");
  double*** func_values;
  if (func_values_alloc(ngrid, &func_values) != 0) {
     perror("Aborting : init_func_values_alloc.\n");
     exit(EXIT_FAILURE);
  }
  if (func_values_init(input_func_filename, ngrid, func_values) != 0) {
     perror("Aborting : init_func_values.\n");
     exit(EXIT_FAILURE);
  }
  printf("Done.\n");
  
  
  /* Init values for the attenuation map on the grid */
  printf("  Initializing attenuation map data...");
  double*** att_values;
  if (att_values_alloc(ngrid, &att_values) != 0) {
     perror("Aborting : init_att_values_alloc.\n");
     exit(EXIT_FAILURE);
  }
  if (att_values_init(input_att_filename, ngrid, att_values) != 0) {	// read attenuation map from file
     perror("Aborting : init_att_values.\n");
     exit(EXIT_FAILURE);
  }
  printf("Done.\n");
  
  
  
  /* Init grid in ray space */
  double *phi, *shift;
  printf("  Initializing the grid in ray transforms space...");
  if (ray_grid_alloc(nshift, nphi, &shift, &phi) != 0) {
     perror("Aborting : init_ray_grid_alloc.\n");
     exit(EXIT_FAILURE);
  }
  if (ray_grid_init(nshift, nphi, shift, phi) != 0 ) {
     perror("Aborting : init_ray_grid.\n");
     exit(EXIT_FAILURE);
  }
  printf("Done.\n\n");
  
  /************************************** END OF INITIALIZATIONS ******************************************/
  
  
  /************************* RADON TRANSFORM WITH PARALLELIZATION VIA OPENMP ******************************/
  //start timer
  struct timeval start, end;
  gettimeofday(&start, NULL);  

  
  /* Init chunks filenames (nchunks = nthreads). chunk filename %d_(output_filename) */
  char** chunks_filenames;
  chunks_filenames_alloc(output_filename, nthreads, &chunks_filenames, FILENAME_MAXSIZE);
  chunks_filenames_init(output_filename, nthreads, chunks_filenames);
  
  // parallelize process with threads
  pthread_t thread[nthreads];
  struct spect_threadf_argument thread_arg[nthreads];
  int i_thread;

  for (i_thread = 0; i_thread < nthreads; ++i_thread) {
      //set equally parts of z-slices for each thread
      int block_size = nslice / nthreads + (i_thread < (nslice % nthreads) ? 1 : 0);
      int z_slice_min = (i_thread < (nslice % nthreads) ? (nslice / nthreads + 1) * i_thread : (nslice / nthreads)*i_thread + (nslice % nthreads)),
          z_slice_max = z_slice_min + block_size; 
      
      thread_arg[i_thread].thread_ID = i_thread;
      thread_arg[i_thread].output_filename = chunks_filenames[i_thread];
      thread_arg[i_thread].z_slice_min = z_slice_min;
      thread_arg[i_thread].z_slice_max = z_slice_max;
      thread_arg[i_thread].nphi = nphi;
      thread_arg[i_thread].nshift = nshift;
      thread_arg[i_thread].nslice = nslice;
      
      thread_arg[i_thread].ngrid = ngrid;
      thread_arg[i_thread].func_values = func_values;
      thread_arg[i_thread].att_values = att_values;
      thread_arg[i_thread].phi = phi;
      thread_arg[i_thread].shift = shift;

      pthread_create(&(thread[i_thread]), NULL, &spect_ray_threadf, &(thread_arg[i_thread]));
  }
  // wait untill threads are done
  for (i_thread = 0; i_thread < nthreads; ++i_thread) {
    pthread_join(thread[i_thread], NULL);
  }

  //stop timer
  gettimeofday(&end, NULL);
  double delta_t = ((end.tv_sec  - start.tv_sec) * 1000000u + 
         end.tv_usec - start.tv_usec) / 1.e6;
  printf("\n");
  printf("  Elapsed time: %3.1f sec\n", nthreads, delta_t);
  
  /************************* END RADON TRANSFORM WITH PARALLELIZATION VIA OPENMP **************************/
  
  
  /*********************************** START AGGREGATING DATA TO ONE FILE *********************************/
  
  printf("  Generating readme...");
  data_generate_readme(output_filename, input_func_filename, input_func_filename, nslice, nshift, nphi);
  printf("  Done.\n");
  printf("  Aggregating chunks to one output file...");
  data_aggregate_chunks(output_filename, chunks_filenames, nthreads);
  printf("Done.\n");
  
  /*********************************** END AGGREGATING DATA TO ONE FILE ***********************************/
  

  /******************************************* CLEANING MEMORY ********************************************/
  
  printf("  Cleaning allocated memory...");
  
  // clean files
  if (chunks_files_clean(nthreads, chunks_filenames) != 0) {
     fprintf(stderr, "Warning : clean_chunks_files.\n");
  }
  if (chunks_filenames_clean(nthreads, chunks_filenames) != 0) {
     fprintf(stderr, "Warning : clean_chunks_filenames.\n");
  }

  
  // clean memory
  if (parameters_clean(parameters) != 0) {
     fprintf(stderr, "Warning : clean_parameters.\n");
  }
  if (func_values_clean(ngrid, func_values) != 0) {	//clean data for the test-function
     fprintf(stderr, "Warning : clean_func_values.\n");
  }
  if (att_values_clean(ngrid, att_values) != 0) {		//clean data for the test-function
     fprintf(stderr, "Warning : clean_att_values.\n");
  }
  if (ray_grid_clean(shift, phi) != 0) {
     fprintf(stderr, "Warning : clean_ray_grid.\n");
  }
  printf("Done.\n");
  
  /******************************************* END CLEANING MEMORY ****************************************/
  
  exit(EXIT_SUCCESS);
}
