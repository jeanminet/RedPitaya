/**
 * $Id: lcr.c 1246  $
 *
 * @brief Red Pitaya LCR meter
 *
 * @Author1 Martin Cimerman (main developer, concept program translation)
 * @Author2 Zumret Topcagic (concept code developer)
 * @Author3 Luka Golinar (functioanlity of web interface)
 * @Author4 Peter Miklavcic (manpage and code review)
 * Contact: <cim.martin@gmail.com>, <luka.golinar@gmail.com>
 *
 * GENERAL DESCRIPTION:
 *
 * The code below defines the LCR meter on a Red Pitaya.
 * It uses acquire and generate from the Test/ folder.
 * Data analysis returns frequency, phase and amplitude and other parameters.
 * 
 * VERSION: VERSION defined in Makefile
 * 
 * This part of code is written in C programming language.
 * Please visit http://en.wikipedia.org/wiki/C_(programming_language)
 * for more details on the language used herein.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <complex.h>
#include <sys/param.h>

#include "main_osc.h"
#include "fpga_osc.h"
#include "fpga_awg.h"
#include "version.h"

#define M_PI 3.14159265358979323846

const char *g_argv0 = NULL; // Program name

const double c_max_frequency = 62.5e6; // Maximal signal frequency [Hz]
const double c_min_frequency = 0; // Minimal signal frequency [Hz]
const double c_max_amplitude = 1.0; // Maximal signal amplitude [V]

#define n (16*1024) // AWG buffer length [samples]
int32_t data[n]; // AWG data buffer

/** Signal types */
typedef enum {
    eSignalSine,         // Sinusoidal waveform
    eSignalSquare,       // Square waveform
    eSignalTriangle,     // Triangular waveform
    eSignalSweep,        // Sinusoidal frequency sweep
	eSignalConst         // Constant signal
} signal_e;

/** AWG FPGA parameters */
typedef struct {
    int32_t  offsgain;   // AWG offset & gain
    uint32_t wrap;       // AWG buffer wrap value
    uint32_t step;       // AWG step interval
} awg_param_t;

/** Oscilloscope module parameters as defined in main module
 * @see rp_main_params
 */
float t_params[PARAMS_NUM] = { 0, 1e6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/** Decimation translation table */
#define DEC_MAX 6 // Max decimation index
static int g_dec[DEC_MAX] = { 1,  8,  64,  1024,  8192,  65536 };

/** Forward declarations */
void synthesize_signal(double ampl, double freq, signal_e type, double endfreq,
                       int32_t *data,
                       awg_param_t *params);
void write_data_fpga(uint32_t ch,
                     const int32_t *data,
                     const awg_param_t *awg);
int acquire_data(float **s ,
                 uint32_t size);
int LCR_data_analysis(float **s,
                      uint32_t size,
                      double DC_bias,
                      double R_shunt,
                      float complex *Z,
                      double w_out,
                      int f);

/** Print usage information */
void usage() {
    const char *format =
            "LCR meter version %s, compiled at %s\n"
            "\n"
            "Usage:\t%s [channel] "
                       "[amplitude] "
                       "[dc bias] "
                       "[r_shunt] "
                       "[averaging] "
                       "[calibration mode] "
                       "[z_ref real] "
                       "[z_ref imag] "
                       "[count/steps] "
                       "[sweep mode] "
                       "[start freq] "
                       "[stop freq] "
                       "[scale type] "
                       "[wait]\n"
            "\n"
            "\tchannel            Channel to generate signal on [1 / 2].\n"
            "\tamplitude          Signal amplitude in V [0 - 1, which means max 2Vpp].\n"
            "\tdc bias            DC bias/offset/component in V [0 - 1].\n"
            "\t                   Max sum of amplitude and DC bias is (0-1]V.\n"
            "\tr_shunt            Shunt resistor value in Ohms [>0].\n"
            "\taveraging          Number of samples per one measurement [>1].\n"
            "\tcalibration mode   0 - none, 1 - open and short, 2 - z_ref.\n"
            "\tz_ref real         Reference impedance, real part.\n"
            "\tz_ref imag         Reference impedance, imaginary part.\n"
            "\tcount/steps        Number of measurements [>1 / >2, dep. on sweep mode].\n"
            "\tsweep mode         0 - measurement sweep, 1 - frequency sweep.\n"
            "\tstart freq         Lower frequency limit in Hz [3 - 62.5e6].\n"
            "\tstop freq          Upper frequency limit in Hz [3 - 62.5e6].\n"
            "\tscale type         0 - linear, 1 - logarithmic.\n"
            "\twait               Wait for user before performing each step [0 / 1].\n"
            "\n"
            "Output:\tfrequency [Hz], phase [deg], Z [Ohm], Y, PhaseY, R_s, X_s, G_p, B_p, C_s, C_p, L_s, L_p, R_p, Q, D\n";

    fprintf(stderr, format, VERSION_STR, __TIMESTAMP__, g_argv0);
}

/* Gain string (lv/hv) to number (0/1) transformation, currently not needed
int get_gain(int *gain, const char *str) {
    if ( (strncmp(str, "lv", 2) == 0) || (strncmp(str, "LV", 2) == 0) ) {
        *gain = 0;
        return 0;
    }
    if ( (strncmp(str, "hv", 2) == 0) || (strncmp(str, "HV", 2) == 0) ) {
        *gain = 1;
        return 0;
    }

    fprintf(stderr, "Unknown gain: %s\n", str);
    return -1;
}
*/

/** Allocates a memory with size num_of_el, memory has 1 dimension */
float *create_table_size(int num_of_el) {
    float *new_table = (float *)malloc( num_of_el * sizeof(float));
    return new_table;
}

/** Allocates a memory with size num_of_cols*num_of_rows */
float **create_2D_table_size(int num_of_rows, int num_of_cols) {
    float **new_table = (float **)malloc( num_of_rows * sizeof(float*));
    int i;
        for(i = 0; i < num_of_rows; i++) {
            new_table[i] = create_table_size(num_of_cols);
        }
    return new_table;
}

/** Finds maximum value from array "arrayptr" with the size of "numofelements" */
float max_array(float *arrayptr, int numofelements) {
  int i = 0;
  float max = -1e6; // Setting the minimum value possible

  for(i = 0; i < numofelements; i++) {
    if(max < arrayptr[ i ]) max = arrayptr[ i ];
  }

  return max;
}

/** Trapezoidal method for integration */
float trapz(float *arrayptr, float T, int size1) {
  float result = 0;
  int i;
  
  for (i =0; i < size1 - 1 ; i++) {
    result += ( arrayptr[i] + arrayptr[ i+1 ]  );
  }
  
  result = (T / (float)2) * result;
  return result;
}

/** Finds a mean value of an array */
float mean_array(float *arrayptr, int numofelements) {
  int i = 1;
  float mean = 0;

  for(i = 0; i < numofelements; i++) {
    mean += arrayptr[i];
  }

  mean = mean / numofelements;
  return mean;
}

/** Finds a mean value of an array by columns, acquiring values from rows */
float mean_array_column(float **arrayptr, int length, int column) {
    float result = 0;
    int i;

    for(i = 0; i < length; i++) {
        result = result + arrayptr[i][column];
    }
    
    result = result / length;
    return result;
}

/** LCR meter  main function it includea all the functionality */
int main(int argc, char *argv[]) {
	
    /** Set program name */
    g_argv0 = argv[0];
    
    /**
     * Manpage
     * 
     * usage() prints its output to stderr, nevertheless main returns
     * zero as calling lcr without any arguments is not an error.
     */
    if (argc==1) {
		usage();
		return 0;
	}
    
    /** Argument check */
    if (argc<15) {
        fprintf(stderr, "Too few arguments!\n\n");
        usage();
        return -1;
    }
    
    /** Argument parsing */
    /// Channel
    unsigned int ch = atoi(argv[1])-1; // Zero-based internally
    if (ch > 1) {
        fprintf(stderr, "Invalid channel value!\n\n");
        usage();
        return -1;
    }
    /// Amplitude
    double ampl = strtod(argv[2], NULL);
    if ( (ampl < 0) || (ampl > c_max_amplitude) ) {
        fprintf(stderr, "Invalid amplitude value!\n\n");
        usage();
        return -1;
    }
    /// DC bias
    double DC_bias = strtod(argv[3], NULL);
    if ( (DC_bias < 0) || (DC_bias > 1) ) {
        fprintf(stderr, "Invalid dc bias value!\n\n");
        usage();
        return -1;
    }
    if ( ampl+DC_bias>1 || ampl+DC_bias<=0 ) {
        fprintf(stderr, "Invalid ampl+dc value!\n\n");
        usage();
        return -1;
    }
    /// Shunt resistor
    double R_shunt = strtod(argv[4], NULL);
    if ( !(R_shunt > 0) ) {
        fprintf(stderr, "Invalid r_shunt value!\n\n");
        usage();
        return -1;
    }
    /// Averaging
    unsigned int averaging_num = strtod(argv[5], NULL);
    if ( averaging_num < 1 ) {
        fprintf(stderr, "Invalid averaging value!\n\n");
        usage();
        return -1;
    }
    /// Calibration mode ( 0 = none, 1 = open&short, 2 = z_ref )
    unsigned int calib_function = strtod(argv[6], NULL);
    if ( calib_function > 2 ) {
        fprintf(stderr, "Invalid calibration mode!\n\n");
        usage();
        return -1;
    }
    /* Reference impedance for calibration purposes */
    /// Z_ref real part
    double Z_load_ref_real = strtod(argv[7], NULL);
    if ( Z_load_ref_real < 0 ) {
        fprintf(stderr, "Invalid z_ref real value!\n\n");
        usage();
        return -1;
    }
    /// Z_ref imaginary part
    double Z_load_ref_imag = strtod(argv[8], NULL);
    /// Count/steps
    unsigned int steps = strtod(argv[9], NULL);
    if ( steps < 1 ) {
        fprintf(stderr, "Invalid count/steps value!\n\n");
        usage();
        return -1;
    }
    /// Sweep mode (0 = measurement, 1 = frequency)
    unsigned int sweep_function = strtod(argv[10], NULL);
    if ( (sweep_function < 0) || (sweep_function > 1) ) {
        fprintf(stderr, "Invalid sweep mode!\n\n");
        usage();
        return -1;
    }
    if ( sweep_function == 1 && steps == 1) {
        fprintf(stderr, "Invalid count/steps value!\n\n");
        usage();
        return -1;
    }
    /// Start frequency for fr.sweep, if measurement sweep selected lcr uses this fr. as main fr.
    double start_frequency = strtod(argv[11], NULL);
    if ( (start_frequency < c_min_frequency) || (start_frequency > c_max_frequency) ) {
        fprintf(stderr, "Invalid start freq!\n\n");
        usage();
        return -1;
    }
    /// end frequency used just for fr.sweep
    double end_frequency = strtod(argv[12], NULL);
    if ( (end_frequency < c_min_frequency) || (end_frequency > c_max_frequency) ) {
        fprintf(stderr, "Invalid end freq!\n\n");
        usage();
        return -1;
    }
    if ( (end_frequency < start_frequency) && (sweep_function == 1) ) {
        fprintf(stderr, "End frequency has to be greater than the start frequency!\n\n");
        usage();
        return -1;
    }

    /// Scale type ( 0 = lin, 1 = log )
    unsigned int scale_type = strtod(argv[13], NULL);
    if ( scale_type > 1 ) {
        fprintf(stderr, "Invalid scale type!\n\n");
        usage();
        return -1;
    }
    /* /// Wait
       The program will wait for user before every measurement when measurement sweep seleced 
       TODO: remove comented code to enable wait function
    */
    unsigned int wait_on_user = strtod(argv[14], NULL);
    if ( wait_on_user > 1 ) {
        fprintf(stderr, "Invalid wait value!\n\n");
        usage();
        return -1;
    }

    /** Parameters initialization and calculation */
    double complex Z_load_ref = Z_load_ref_real + Z_load_ref_imag*I;
    double frequency_steps_number, frequency_step, a, b, c;// a,b and c used for logaritmic scale functionality
    double   measurement_sweep;
    double   w_out; // angular velocity
    double   endfreq = 0; // endfreq set for generate's sweep
    signal_e type = eSignalSine;
    float    log_Frequency;
    float    **s = create_2D_table_size(SIGNALS_NUM, SIGNAL_LENGTH); // raw acquired data saved to this location
    uint32_t min_periodes = 10; // max 20
    // when frequency lies below 100Hz number of acquired periodes reduces to 2
    // this reduces measurement time
    if (start_frequency < 100 && sweep_function == 0) {
        min_periodes = 2;
    }
    uint32_t size; // number of samples varies with number of periodes
    int      dimension_step = 0; // saving data on the right place in allocated memory this is iterator
    int    measurement_sweep_user_defined;
    int    end_results_dimension;
    int      f = 0; // used in for lop, setting the decimation
    int      i, i1, fr, h; // iterators in for loops
    int      equal = 0; // parameter initialized for generator functionality
    int      shaping = 0; // parameter initialized for generator functionality
    int transientEffectFlag = 1;
    int stepsTE = 10; // number of steps for transient effect(TE) elimination
    int TE_step_counter;
    int progress_int = 0;
    char command[70];
    char hex[45];
    // if user sets less than 10 steps than stepsTE is decresed
    // for transient efect to be eliminated only 10 steps of measurements is eliminated
    if (steps < 10){
        stepsTE = steps;
    }
    TE_step_counter = stepsTE;
    
    /// If logarythmic scale is selected start and end frequencies are defined to compliment logaritmic output
    if ( scale_type ) {
        a = log10(start_frequency);
        b = log10(end_frequency);
        if (steps==1) { // Preventing division by zero.
			c = (b - a);
	    } else {
			c = (b - a)/(steps - 1);
		}
    }
    /// Based on which sweep mode is selected some for loops have to iterate only once
    if ( sweep_function ){ // Frequency sweep
        measurement_sweep = 1;
		measurement_sweep_user_defined = 1;
        frequency_steps_number = steps;
        if (steps==1) { // Preventing division by zero.
			frequency_step = ( end_frequency - start_frequency );
		} else {
			frequency_step = ( end_frequency - start_frequency ) / ( frequency_steps_number - 1 );
		}
    }
    else { // Measurement sweep
        measurement_sweep_user_defined = steps;
		frequency_steps_number = 2; // the first one is for removing 
        frequency_step = 0;
    }
    /// Allocated memory size depends on the sweep function
    if ( sweep_function ){ // Frequency sweep
		end_results_dimension = frequency_steps_number;
    }
    else { // Measurement sweep
        end_results_dimension = measurement_sweep_user_defined;
    }  

    /** Memory allocation */

    float complex *Z = (float complex *)malloc( (averaging_num + 1) * sizeof(float complex));
    if (Z == NULL){
        fprintf(stderr,"error allocating memory for complex Z\n");
        return -1;
    }

    float **Calib_data_short_for_averaging = create_2D_table_size((averaging_num + 1), 2);
    if (Calib_data_short_for_averaging == NULL){
        fprintf(stderr,"error allocating memory for Calib_data_short_for_averaging\n");
        return -1;
    }
    float **Calib_data_short  = create_2D_table_size(measurement_sweep_user_defined, 2);
    if (Calib_data_short == NULL){
        fprintf(stderr,"error allocating memory for Calib_data_short\n");
        return -1;
    }

    float **Calib_data_open_for_averaging = create_2D_table_size((averaging_num + 1), 2);
    if (Calib_data_open_for_averaging == NULL){
        fprintf(stderr,"error allocating memory for Calib_data_open_for_averaging\n");
        return -1;
    }
    float **Calib_data_open = create_2D_table_size(measurement_sweep_user_defined, 2);
    if (Calib_data_open == NULL){
        fprintf(stderr,"error allocating memory for Calib_data_open\n");
        return -1;
    }

    float **Calib_data_load_for_averaging = create_2D_table_size((averaging_num + 1), 2);
    if (Calib_data_load_for_averaging == NULL){
        fprintf(stderr,"error allocating memory for Calib_data_load_for_averaging\n");
        return -1;
    }
    float **Calib_data_load = create_2D_table_size(measurement_sweep_user_defined, 2);
    if (Calib_data_load == NULL){
        fprintf(stderr,"error allocating memory for Calib_data_load\n");
        return -1;
    }

    float **Calib_data_measure_for_averaging = create_2D_table_size((averaging_num + 1), 2 );
    if (Calib_data_measure_for_averaging == NULL){
        fprintf(stderr,"error allocating memory for Calib_data_measure_for_averaging\n");
        return -1;
    }
    float **Calib_data_measure = create_2D_table_size(measurement_sweep_user_defined, 4);
    if (Calib_data_measure == NULL){
        fprintf(stderr,"error allocating memory for Calib_data_measure\n");
        return -1;
    }
    
    float complex *Z_short    = (float complex *)malloc( end_results_dimension * sizeof(float complex));
    if (Z_short == NULL){
        fprintf(stderr,"error allocating memory for Z_short\n");
        return -1;
    }
    float complex *Z_open     = (float complex *)malloc( end_results_dimension * sizeof(float complex));
    if (Z_open == NULL){
        fprintf(stderr,"error allocating memory for Z_open\n");
        return -1;
    }
    float complex *Z_load     = (float complex *)malloc( end_results_dimension * sizeof(float complex));
    if (Z_load == NULL){
        fprintf(stderr,"error allocating memory for Z_load\n");
        return -1;
    }
    float complex *Z_measure  = (float complex *)malloc( end_results_dimension * sizeof(float complex));
    if (Z_measure == NULL){
        fprintf(stderr,"error allocating memory for Z_measure\n");
        return -1;
    }

    float *calib_data_combine = (float *)malloc( 2 * sizeof(float)); // 0=f, 1=Zreal, 2=Zimag
    if (calib_data_combine == NULL){
        fprintf(stderr,"error allocating memory for calib_data_combine\n");
        return -1;
    }
    float complex *Z_final  = (float complex *)malloc( end_results_dimension * sizeof(float complex));
    if (Z_final == NULL){
        fprintf(stderr,"error allocating memory for Z_final\n");
        return -1;
    }

    float *PhaseZ = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (PhaseZ == NULL){
        fprintf(stderr,"error allocating memory for PhaseZ\n");
        return -1;
    }

    float *PhaseY = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (PhaseY == NULL){
        fprintf(stderr,"error allocating memory for PhaseY\n");
        return -1;
    }

    float *AmplitudeZ = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (AmplitudeZ == NULL){
        fprintf(stderr,"error allocating memory for AmplitudeZ\n");
        return -1;
    }

    float *Frequency = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (Frequency == NULL){
        fprintf(stderr,"error allocating memory for Frequency\n");
        return -1;
    }

    float *R_s = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (R_s == NULL){
        fprintf(stderr,"error allocating memory for R_s\n");
        return -1;
    }

    float *X_s = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (X_s == NULL){
        fprintf(stderr,"error allocating memory for X_s\n");
        return -1;
    }

    float complex *Y  = (float complex *)malloc( end_results_dimension * sizeof(float complex));
    if (Y == NULL){
        fprintf(stderr,"error allocating memory for Z_final\n");
        return -1;
    }
    

    float *Y_abs  = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (Y_abs == NULL){
        fprintf(stderr,"error allocating memory for Y_abs\n");
        return -1;
    }

    float *G_p = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (G_p == NULL){
        fprintf(stderr,"error allocating memory for G_p\n");
        return -1;
    }

    float *B_p = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (B_p == NULL){
        fprintf(stderr,"error allocating memory for B_p\n");
        return -1;
    }

    float *C_s = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (C_s == NULL){
        fprintf(stderr,"error allocating memory for C_s\n");
        return -1;
    }

    float *C_p = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (C_p == NULL){
        fprintf(stderr,"error allocating memory for C_p\n");
        return -1;
    }

    float *L_s = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (L_s == NULL){
        fprintf(stderr,"error allocating memory for L_s\n");
        return -1;
    }

    float *L_p = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (L_p == NULL){
        fprintf(stderr,"error allocating memory for L_p\n");
        return -1;
    }

    float *R_p = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (R_p == NULL){
        fprintf(stderr,"error allocating memory for R_p\n");
        return -1;
    }

    float *Q = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (Q == NULL){
        fprintf(stderr,"error allocating memory for Q\n");
        return -1;
    }

    float *D = (float *)malloc((end_results_dimension + 1) * sizeof(float) );
    if (D == NULL){
        fprintf(stderr,"error allocating memory for D\n");
        return -1;
    }

    /* Initialization of Oscilloscope application */
    if(rp_app_init() < 0) {
        fprintf(stderr, "rp_app_init() failed!\n");
        return -1;
    }
    
    /** User is inquired to correctly set the connections. */
    /*
    if (inquire_user_wait() < 0) {
        printf("error user inquiry at inquire_user_wait\n");
    } 
    */

    /* 
    * for loop defines measurement purpose switching
    * there are 4 sorts of measurement purposes , 3 pof them reprisent calibration sequence
    * [h=0] - calibration open connections, [h=1] - calibration short circuited, [h=2] calibration load, [h=3] actual measurment  
    */
    //FILE *progress_file = fopen("/tmp/lcr_data/progress.txt", "w");
    for (h = 0; h <= 3 ; h++) {
        if (!calib_function) {
            h = 3;
        }
        /* 
        * for floop dedicated to run through the frequency range defined by user
        * the loop also includes the start and end frequency  
        */
        for ( fr = 0; fr < frequency_steps_number; fr++ ) {

            if ( scale_type ) { // log scle
                log_Frequency = powf( 10, ( c * (float)fr ) + a );
                Frequency[ fr ] =  (int)log_Frequency ;
            }
            else { // lin scale
                Frequency[ fr ] = (int)(start_frequency + ( frequency_step * fr ));
            }



            // eliminates transient effect that spoiles the measuremets
            // it outputs frequencies below start frequency and increses 
            // it to the strat frequency
            if (sweep_function == 1 && transientEffectFlag == 1){
                if (TE_step_counter > 0){
                    Frequency[fr] = (int)(start_frequency - (start_frequency/2) + ((start_frequency/2)*TE_step_counter/stepsTE) );
                    TE_step_counter--;
                }
                if (TE_step_counter == 0){
                    fr = 0;
                    Frequency[fr] = start_frequency;
                    transientEffectFlag = 0;
                }
                
            }

            //measurement sweep transient effect
            else if(sweep_function == 0 && transientEffectFlag == 1 ){

                //printf("stepsTE = %d\n",stepsTE);
                if (TE_step_counter > 0){
                    measurement_sweep = TE_step_counter;
                    TE_step_counter = 0;
                }
                else if (TE_step_counter == 0){
                    transientEffectFlag = 0;
                    measurement_sweep = measurement_sweep_user_defined;
                    //printf(" measurement sweep = %f\n",measurement_sweep);
                }
            }


            
            
            w_out = Frequency[ fr ] * 2 * M_PI; // omega - angular velocity

            /* Signal generator
             * fills the vector with amplitude values and then sends it to fpga buffer  */
            awg_param_t params;
            /* Prepare data buffer (calculate from input arguments) */
            synthesize_signal( ampl, Frequency[fr], type, endfreq, data, &params );
            /* Write the data to the FPGA and set FPGA AWG state machine */
            write_data_fpga( ch, data, &params );

            /* TODO calibration sequence parameters adjustments
            // if measurement sweep selected, only one calibration measurement is made 
            if (sweep_function == 0 ) { // sweep_function == 0 (mesurement sweep)
                if (h == 0 || h == 1|| h == 2) {
                    measurement_sweep = 1;
                }
                else {// when gathering measurement results sweep is defined by argument for num of steps
                    measurement_sweep = measurement_sweep_user_defined;
                }
            }
            else if (sweep_function == 1) { // sweep_function == 1 (frequency sweep)
                measurement_sweep = 1; //when frequency sweep selected only one measurement for each fr is made
            }*/
            /*
            * Measurement sweep defined by user, if frequency sweep is used, the for loop goes through only once
            */
            for (i = 0; i < measurement_sweep; i++ ) {
                
                /*Opens, empties a file and inuts the prorgress number*/
                if(sweep_function == 0 ){
                    //printf("transient flag = %d\n", transientEffectFlag);
                    if(transientEffectFlag == 1){
                        progress_int = (int)(100*(  ( i)   / (measurement_sweep_user_defined + stepsTE -1)));
                        
                    }
                    else if (transientEffectFlag == 0){
                        progress_int = (int)(100*( (i + stepsTE)  / (measurement_sweep + stepsTE - 1 )));
                    }
                }
                else if(sweep_function == 1 ){
                    if (TE_step_counter > 0){
                        progress_int = (int)(100*(( stepsTE - TE_step_counter )/( frequency_steps_number + stepsTE - 1 )) );
                    }
                    else  if (TE_step_counter < 1){
                        progress_int = (int)(100*((fr + stepsTE  )/( frequency_steps_number + stepsTE - 1 )) );
                    }

                }

                // writing data to a file
                FILE *progress_file = fopen("/tmp/progress", "w");
                if (progress_int <= 100){
                    fprintf(progress_file , "%d \n" ,  progress_int );

                    sprintf(hex, "%x", (int)(255 - (255*progress_int/100)));
                    strcpy(command, "/opt/bin/monitor 0x40000030 0x" );
                    strcat(command, hex);
                    
                    system(command);
                    fprintf(progress_file , "%d \n" ,  progress_int );
                    //system("clear");
                    //printf(" progress: %d  \n",progress_int);
                    
                    fclose(progress_file);
                }
                
                
                
                for ( i1 = 0; i1 < averaging_num; i1++ ) {

                    /* decimation changes depending on frequency */
                    if      (Frequency[ fr ] >= 160000){      f = 0;    }
                    else if (Frequency[ fr ] >= 20000) {      f = 1;    }    
                    else if (Frequency[ fr ] >= 2500)  {      f = 2;    }    
                    else if (Frequency[ fr ] >= 160)   {      f = 3;    }    
                    else if (Frequency[ fr ] >= 20)    {      f = 4;    }     
                    else if (Frequency[ fr ] >= 2.5)   {      f = 5;    }

                    /* setting decimtion */
                    if (f != DEC_MAX) {
                        t_params[TIME_RANGE_PARAM] = f;
                    } else {
                        fprintf(stderr, "Invalid decimation DEC\n");
                        usage();
                        return -1;
                    }
                    
                    /* calculating num of samples */
                    size = round( ( min_periodes * 125e6 ) / ( Frequency[ fr ] * g_dec[ f ] ) );

                    /* Filter parameters for signal Acqusition */
                    t_params[EQUAL_FILT_PARAM] = equal;
                    t_params[SHAPE_FILT_PARAM] = shaping;

                    /* Setting of parameters in Oscilloscope main module for signal Acqusition */
                    if(rp_set_params((float *)&t_params, PARAMS_NUM) < 0) {
                        fprintf(stderr, "rp_set_params() failed!\n");
                        return -1;
                    }

                    /* Data acqusition function, data saved to s */
                    if (acquire_data(s, size) < 0) {
                        printf("error acquiring data @ acquire_data\n");
                        return -1;
                    }

                    /* Data analyzer, saves darta to Z (complex impedance) */
                    if( LCR_data_analysis( s, size, DC_bias, R_shunt, Z, w_out, f ) < 0) {
                        printf("error data analysis LCR_data_analysis\n");
                        return -1;
                    }

                    /* Saving data for averaging here all the data is saved the dimention of memmory allocated
                     * depends on averaging argument user sets
                    */
                    switch ( h ) {
                    case 0:
                        Calib_data_short_for_averaging[ i1 ][ 1 ] = creal(*Z);
                        Calib_data_short_for_averaging[ i1 ][ 2 ] = cimag(*Z);
                        break;
                    case 1:
                        Calib_data_open_for_averaging[ i1 ][ 1 ] = creal(*Z);
                        Calib_data_open_for_averaging[ i1 ][ 2 ] = cimag(*Z);
                        break;
                    case 2:
                        Calib_data_load_for_averaging[ i1 ][ 1 ] = creal(*Z);
                        Calib_data_load_for_averaging[ i1 ][ 2 ] = cimag(*Z);
                        break;
                    case 3:
                        Calib_data_measure_for_averaging[ i1 ][ 1 ] = creal(*Z);
                        Calib_data_measure_for_averaging[ i1 ][ 2 ] = cimag(*Z);
                        break;
                    default:
                        printf("error no function set for h = %d, when saving data\n", h);
                    }

                } // averaging loop ends here

                /* Calculating and saving mean values */
                switch ( h ) {
                case 0:
                    Calib_data_short[ i ][ 1 ] = mean_array_column( Calib_data_short_for_averaging, averaging_num, 1);
                    Calib_data_short[ i ][ 2 ] = mean_array_column( Calib_data_short_for_averaging, averaging_num, 2);
                    break;
                case 1:
                    Calib_data_open[ i ][ 1 ] = mean_array_column(Calib_data_open_for_averaging, averaging_num, 1);
                    Calib_data_open[ i ][ 2 ] = mean_array_column(Calib_data_open_for_averaging, averaging_num, 2);
                    break;
                case 2:
                    Calib_data_load[ i ][ 1 ] = mean_array_column(Calib_data_load_for_averaging, averaging_num, 1);
                    Calib_data_load[ i ][ 2 ] = mean_array_column(Calib_data_load_for_averaging, averaging_num, 2);
                    break;
                case 3:
                    Calib_data_measure[ i ][ 1 ] = mean_array_column( Calib_data_measure_for_averaging, averaging_num, 1 );
                    Calib_data_measure[ i ][ 2 ] = mean_array_column( Calib_data_measure_for_averaging, averaging_num, 2 );
                    break;
                default:
                    printf("error no function set for h = %d, when averaging data\n", h);
                }

                /* dimension step defines index for sorting data depending on sweep function */
                if (sweep_function == 0 ) { //sweep_function == 0 (mesurement sweep)
                    dimension_step = i;
                }
                else if(sweep_function == 1) { //sweep_function == 1 (frequency sweep)
                   dimension_step = fr;
                }
                
                /* Saving data for output */
                //printf("Frequency(%d) = %f;\n",(dimension_step),Frequency[fr]);
                /* vector must be populated with the same values when measuremen sweep selected*/
                Z_short[ dimension_step ] =  Calib_data_short[ 0 ][ 1 ] + Calib_data_short[ 0 ][ 2 ] *I;
                Z_open[ dimension_step ]  =  Calib_data_open[ 0 ][ 1 ] + Calib_data_open[ 0 ][ 2 ] *I;
                Z_load[ dimension_step ]  =  Calib_data_load[ 0 ][ 1 ] + Calib_data_load[ 0 ][ 2 ] *I;

                Z_measure[dimension_step] = Calib_data_measure[i][1] + Calib_data_measure[i][2] *I;

            } // measurement sweep loop ends here
        
        } // frequency sweep loop ends here

    } // function step loop ends here

    /* Setting amplitude to 0V - turning off the output. */
    awg_param_t params;
    /* Prepare data buffer (calculate from input arguments) */
    synthesize_signal( 0, 1000, type, endfreq, data, &params );
    /* Write the data to the FPGA and set FPGA AWG state machine */
    write_data_fpga( ch, data, &params );

    /** User is inquired to correcty set the connections. */
    /*
    if (inquire_user_wait() < 0) {
        printf("error user inquiry at inquire_user_wait\n");
    } 
    */

    /** Opening frequency data */
    FILE *try_open = fopen("/tmp/lcr_data/data_frequency", "w");

    /* If files don't exists yet, we first have to create them (First boot), as we are storing them in /tmp */
    if(try_open == NULL){

        int f_number;
        char command[100];

        strcpy(command, "mkdir /tmp/lcr_data");
        system(command);

        /* We loop X (Where X is the number of data we want to have) times and create a file for each data type */
        for(f_number = 0; f_number < 16; f_number++){
            switch(f_number){
                case 0:
                    strcpy(command, "touch /tmp/lcr_data/data_frequency");
                    system(command);
                    break;
                case 1:
                    strcpy(command, "touch /tmp/lcr_data/data_amplitude");
                    system(command);
                    break;
                case 2:
                    strcpy(command, "touch /tmp/lcr_data/data_phase");
                    system(command);
                    break;
                case 3:
                    strcpy(command, "touch /tmp/lcr_data/data_R_s");
                    system(command);
                    break;
                case 4:
                    strcpy(command, "touch /tmp/lcr_data/data_X_s");
                    system(command);
                    break;
                case 5:
                    strcpy(command, "touch /tmp/lcr_data/data_G_p");
                    system(command);
                    break;
                case 6:
                    strcpy(command, "touch /tmp/lcr_data/data_B_p");
                    system(command);
                    break;
                case 7:
                    strcpy(command, "touch /tmp/lcr_data/data_C_s");
                    system(command);
                    break;
                case 8:
                    strcpy(command, "touch /tmp/lcr_data/data_C_p");
                    system(command);
                    break;
                case 9:
                    strcpy(command, "touch /tmp/lcr_data/data_L_s");
                    system(command);
                    break;
                case 10:
                    strcpy(command, "touch /tmp/lcr_data/data_L_p");
                    system(command);
                    break;
                case 11:
                    strcpy(command, "touch /tmp/lcr_data/data_R_p");
                    system(command);
                    break;
                case 12:
                    strcpy(command, "touch /tmp/lcr_data/data_Q");
                    system(command);
                    break;
                case 13:
                    strcpy(command, "touch /tmp/lcr_data/data_D");
                    system(command);
                    break;
                case 14:
                    strcpy(command, "touch /tmp/lcr_data/data_Y_abs");
                    system(command);
                    break;
                case 15:
                    strcpy(command, "touch /tmp/lcr_data/data_phaseY");
                    system(command);
                    break;

            }
        }
        /* We change the mode to write and add permission. */
        strcpy(command, "chmod -R 777 /tmp/lcr_data");
        
        /* Command execution for before the next loop */
        system(command);
    }

    /** Opening files */
    FILE *file_frequency = fopen("/tmp/lcr_data/data_frequency", "w");
    FILE *file_phase = fopen("/tmp/lcr_data/data_phase", "w");
    FILE *file_amplitude = fopen("/tmp/lcr_data/data_amplitude", "w");
    FILE *file_Y_abs = fopen("/tmp/lcr_data/data_Y_abs", "w");
    FILE *file_PhaseY = fopen("/tmp/lcr_data/data_phaseY", "w");
    FILE *file_R_s = fopen("/tmp/lcr_data/data_R_s", "w");
    FILE *file_X_s = fopen("/tmp/lcr_data/data_X_s", "w");
    FILE *file_G_p = fopen("/tmp/lcr_data/data_G_p", "w");
    FILE *file_B_p = fopen("/tmp/lcr_data/data_B_p", "w");
    FILE *file_C_s = fopen("/tmp/lcr_data/data_C_s", "w");
    FILE *file_C_p = fopen("/tmp/lcr_data/data_C_p", "w");
    FILE *file_L_s = fopen("/tmp/lcr_data/data_L_s", "w");
    FILE *file_L_p = fopen("/tmp/lcr_data/data_L_p", "w");
    FILE *file_R_p = fopen("/tmp/lcr_data/data_R_p", "w");
    FILE *file_Q = fopen("/tmp/lcr_data/data_Q", "w");
    FILE *file_D = fopen("/tmp/lcr_data/data_D", "w");


    /** Combining all the data and printing it to stdout 
     * depending on calibration argument output data is calculated
     */
    for ( i = 0; i < end_results_dimension ; i++ ) {

        if ( calib_function == 1 ) { // calib. was made including Z_load
            calib_data_combine[ 1 ] = creal( ( ( ( Z_short[ i ] - Z_measure[ i ]) * (Z_load[ i ] - Z_open[ i ]) ) / ( (Z_measure[i] - Z_open[i]) * (Z_short[i] - Z_load[i]) ) ) * Z_load_ref );
            calib_data_combine[ 2 ] = cimag( ( ( ( Z_short[ i ] - Z_measure[ i ]) * (Z_load[ i ] - Z_open[ i ]) ) / ( (Z_measure[i] - Z_open[i]) * (Z_short[i] - Z_load[i]) ) ) * Z_load_ref );
        }

        else if ( calib_function == 0 ) { // no calib. were made, outputing data from measurements
            calib_data_combine[ 1 ] = creal( Z_measure[ i ]);
            calib_data_combine[ 2 ] = cimag( Z_measure[ i ]);
        }

        else if ( calib_function == 2 ) { // calibration without Z_load
            calib_data_combine[ 1 ] = creal( ( ( ( Z_short[i] - Z_measure[i]) * ( Z_open[i]) ) / ( (Z_measure[i] - Z_open[i]) * (Z_short[i] - Z_load[i]) ) ) );
            calib_data_combine[ 2 ] = cimag( ( ( ( Z_short[i] - Z_measure[i]) * ( Z_open[i]) ) / ( (Z_measure[i] - Z_open[i]) * (Z_short[i] - Z_load[i]) ) ) );
        }
        w_out = 2 * M_PI * Frequency[ i ]; // angular velocity
        
        PhaseZ[ i ] = ( 180 / M_PI) * (atan2f( calib_data_combine[ 2 ], calib_data_combine[ 1 ] ));
        AmplitudeZ[ i ] = sqrtf( powf( calib_data_combine[ 1 ], 2 ) + powf(calib_data_combine[ 2 ], 2 ) );

        Z_final[ i ] = calib_data_combine[ 1 ] + calib_data_combine[ 2 ]*I;//Z=Z_abs*exp(i*Phase_rad);;
        R_s[ i ] =  calib_data_combine[ 1 ];//R_s=real(Z);
        X_s[ i ] = calib_data_combine[ 2 ];//X_s=imag(Z);
        
        Y [ i ] = 1 / Z_final[ i ];//Y=1/Z;
        Y_abs [ i ] = sqrtf( powf( creal(Y[ i ]), 2 ) + powf(cimag(Y[ i ]), 2 ) );//Y_abs=abs(Y);
        PhaseY[ i ] = -PhaseZ[ i ];// PhaseY=-Phase_rad;
        G_p [ i ] = creal(Y[ i ]);//G_p=real(Y);;
        B_p [ i ] = cimag(Y[ i ]);//B_p=imag(Y);
        
        C_s [ i ] = -1 / (w_out * X_s[ i ]);//C_s=-1/(w*X_s);
        C_p [ i ] = B_p[ i ] / w_out;//C_p=B_p/w; //inf
        L_s [ i ] = X_s[ i ] / w_out;//L_s=X_s/w; //inf
        L_p [ i ] = -1 / (w_out * B_p[ i ]);//L_p=-1/(w*B_p); //inf
        R_p [ i ] = 1 / G_p [ i ]; //R_p=1/G_p;
        
        Q[ i ] =X_s[ i ] / R_s[ i ]; //Q=X_s/R_s;
        D[ i ] = -1 / Q [ i ]; //D=-1/Q;
        
        /// Output
        if ( !sweep_function ) {
            printf(" %.2f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f\n",
                Frequency[ 0 ],
                PhaseZ[ i ],
                AmplitudeZ[ i ],
                Y_abs[ i ],
                PhaseY[ i ],
                R_s[ i ],
                X_s[ i ],
                G_p[ i ],
                B_p[ i ],
                C_s[ i ],
                C_p[ i ],
                L_s[ i ],
                L_p[ i ],
                R_p[ i ],
                Q[ i ],
                D[ i ]
                );
        } else {
            printf(" %.0f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f    %.5f\n",
                Frequency[ i ],
                PhaseZ[ i ],
                AmplitudeZ[ i ],
                Y_abs[ i ],
                PhaseY[ i ],
                R_s[ i ],
                X_s[ i ],
                G_p[ i ],
                B_p[ i ],
                C_s[ i ],
                C_p[ i ],
                L_s[ i ],
                L_p[ i ],
                R_p[ i ],
                Q[ i ],
                D[ i ]
                );
        }
        
        /** Saving files */
        if (!sweep_function) {
            fprintf(file_frequency, "%.5f\n", Frequency[0]);
        } else {
            fprintf(file_frequency, "%.5f\n", Frequency[i]);
        }
        
        fprintf(file_phase, "%.5f\n", PhaseZ[i]);
        fprintf(file_amplitude, "%.5f\n", AmplitudeZ[i]);
        fprintf(file_R_s, "%.5f\n", R_s[i]);
        fprintf(file_Y_abs, "%.5f\n", Y_abs[i]);
        fprintf(file_PhaseY, "%.5f\n", PhaseY[i]);
        fprintf(file_X_s, "%.5f\n", X_s[i]);
        fprintf(file_G_p, "%.5f\n", G_p[i]);
        fprintf(file_B_p, "%.5f\n", B_p[i]);
        fprintf(file_C_s, "%.5f\n", C_s[i]);
        fprintf(file_C_p, "%.5f\n", C_p[i]);
        fprintf(file_L_s, "%.5f\n", L_s[i]);
        fprintf(file_L_p, "%.5f\n", L_p[i]);
        fprintf(file_R_p, "%.5f\n", R_p[i]);
        fprintf(file_Q, "%.5f\n", Q[i]);
        fprintf(file_D, "%.5f\n", D[i]);
    }

    /** Closing files */
    fclose(file_frequency);
    fclose(file_phase);
    fclose(file_amplitude);
    fclose(file_R_s);
    fclose(file_Y_abs);
    fclose(file_PhaseY);
    fclose(file_X_s);
    fclose(file_G_p);
    fclose(file_B_p);
    fclose(file_C_s);
    fclose(file_C_p);
    fclose(file_L_s);
    fclose(file_L_p);
    fclose(file_R_p);
    fclose(file_Q);
    fclose(file_D);

    /** All's well that ends well. */
    return 1;

}

/**
 * Synthesize a desired signal.
 *
 * Generates/synthesized  a signal, based on three pre-defined signal
 * types/shapes, signal amplitude & frequency. The data[] vector of 
 * samples at 125 MHz is generated to be re-played by the FPGA AWG module.
 *
 * @param ampl  Signal amplitude [V].
 * @param freq  Signal Frequency [Hz].
 * @param type  Signal type/shape [Sine, Square, Triangle, Constant].
 * @param data  Returned synthesized AWG data vector.
 * @param awg   Returned AWG parameters.
 *
 */
void synthesize_signal(double ampl, double freq, signal_e type, double endfreq,
                       int32_t *data,
                       awg_param_t *awg) {

    uint32_t i;

    /* Various locally used constants - HW specific parameters */
    const int dcoffs = -155;
    const int trans0 = 30;
    const int trans1 = 300;
    const double tt2 = 0.249;

    /* This is where frequency is used... */
    awg->offsgain = (dcoffs << 16) + 0x1fff;
    awg->step = round(65536 * freq/c_awg_smpl_freq * n);
    awg->wrap = round(65536 * (n-1));

    int trans = freq / 1e6 * trans1; /* 300 samples at 1 MHz */
    uint32_t amp = ampl * 4000.0;    /* 1 V ==> 4000 DAC counts */
    if (amp > 8191) {
        /* Truncate to max value if needed */
        amp = 8191;
    }

    if (trans <= 10) {
        trans = trans0;
    }


    /* Fill data[] with appropriate buffer samples */
    for(i = 0; i < n; i++) {
        
        /* Sine */
        if (type == eSignalSine) {
            data[i] = round(amp * cos(2*M_PI*(double)i/(double)n));
        }
 
        /* Square */
        if (type == eSignalSquare) {
            data[i] = round(amp * cos(2*M_PI*(double)i/(double)n));
            if (data[i] > 0)
                data[i] = amp;
            else 
                data[i] = -amp;

            /* Soft linear transitions */
            double mm, qq, xx, xm;
            double x1, x2, y1, y2;    

            xx = i;       
            xm = n;
            mm = -2.0*(double)amp/(double)trans; 
            qq = (double)amp * (2 + xm/(2.0*(double)trans));
            
            x1 = xm * tt2;
            x2 = xm * tt2 + (double)trans;
            
            if ( (xx > x1) && (xx <= x2) ) {  
                
                y1 = (double)amp;
                y2 = -(double)amp;
                
                mm = (y2 - y1) / (x2 - x1);
                qq = y1 - mm * x1;

                data[i] = round(mm * xx + qq); 
            }
            
            x1 = xm * 0.75;
            x2 = xm * 0.75 + trans;
            
            if ( (xx > x1) && (xx <= x2)) {  
                    
                y1 = -(double)amp;
                y2 = (double)amp;
                
                mm = (y2 - y1) / (x2 - x1);
                qq = y1 - mm * x1;
                
                data[i] = round(mm * xx + qq); 
            }
        }
        
        /* Triangle */
        if (type == eSignalTriangle) {
            data[i] = round(-1.0*(double)amp*(acos(cos(2*M_PI*(double)i/(double)n))/M_PI*2-1));
        }

        /* Sweep */
        /* Loops from i = 0 to n = 16*1024. Generates a sine wave signal that
           changes in frequency as the buffer is filled. */
        double start = 2 * M_PI * freq;
        double end = 2 * M_PI * endfreq;
        if (type == eSignalSweep) {
            double sampFreq = c_awg_smpl_freq; // 125 MHz
            double t = i / sampFreq; // This particular sample
            double T = n / sampFreq; // Wave period = # samples / sample frequency
            /* Actual formula. Frequency changes from start to end. */
            data[i] = round(amp * (sin((start*T)/log(end/start) * ((exp(t*log(end/start)/T)-1)))));
        }
        
        /* Constant */
		if (type == eSignalConst) data[i] = amp;
        
        /* TODO: Remove, not necessary in C/C++. */
        if(data[i] < 0)
            data[i] += (1 << 14);
    }
}

/**
 * Write synthesized data[] to FPGA buffer.
 *
 * @param ch    Channel number [0, 1].
 * @param data  AWG data to write to FPGA.
 * @param awg   AWG paramters to write to FPGA.
 */
void write_data_fpga(uint32_t ch,
                     const int32_t *data,
                     const awg_param_t *awg) {

    uint32_t i;

    fpga_awg_init();

    if(ch == 0) {
        /* Channel A */
        g_awg_reg->state_machine_conf = 0x000041;
        g_awg_reg->cha_scale_off      = awg->offsgain;
        g_awg_reg->cha_count_wrap     = awg->wrap;
        g_awg_reg->cha_count_step     = awg->step;
        g_awg_reg->cha_start_off      = 0;

        for(i = 0; i < n; i++) {
            g_awg_cha_mem[i] = data[i];
        }
    } else {
        /* Channel B */
        g_awg_reg->state_machine_conf = 0x410000;
        g_awg_reg->chb_scale_off      = awg->offsgain;
        g_awg_reg->chb_count_wrap     = awg->wrap;
        g_awg_reg->chb_count_step     = awg->step;
        g_awg_reg->chb_start_off      = 0;

        for(i = 0; i < n; i++) {
            g_awg_chb_mem[i] = data[i];
        }
    }

    /* Enable both channels */
    /* TODO: Should this only happen for the specified channel?
     *       Otherwise, the not-to-be-affected channel is restarted as well
     *       causing unwanted disturbances on that channel.
     */
    g_awg_reg->state_machine_conf = 0x110011;

    fpga_awg_exit();
}

/**
 * Acquire data from FPGA to memory (s).
 *
 * @param **s   Points to a memory where data is saved.
 * @param size  Size of data.
 */
int acquire_data(float **s ,
                uint32_t size) {
    int retries = 150000;
    int j, sig_num, sig_len;
    int ret_val;
    usleep(50000);
    while(retries >= 0) {
        if((ret_val = rp_get_signals(&s, &sig_num, &sig_len)) >= 0) {
            /* Signals acquired in s[][]:
             * s[0][i] - TODO
             * s[1][i] - Channel ADC1 raw signal
             * s[2][i] - Channel ADC2 raw signal
             */
            for(j = 0; j < MIN(size, sig_len); j++) {
                //printf("%7d, %7d\n",(int)s[1][j], (int)s[2][j]);
            }
            break;
        }
        if(retries-- == 0) {
            fprintf(stderr, "Signal scquisition was not triggered!\n");
            break;
        }
        usleep(1000);
    }
    usleep(30000); // delay for pitaya to operate correctly
    return 1;
}

/**
 * Acquired data analysis function for LCR meter.
 * returnes 1 if execution was successful
 * the function recives data stored in memmory with the pointer "s"
 * data manipulation returnes Phase and Amplitude which is at the end 
 * transformed to complex impedance.
 * 
 * @param s        Pointer where data is read from.
 * @param size     Size of data.
 * @param DC_bias  DC component.
 * @param R_shunt  Shunt resistor value in Ohms.
 * @param Z        Pointer where to write impedance data (in complex form).
 * @param w_out    Angular velocity (2*pi*freq).
 * @param f        Decimation selector index.
 */
int LCR_data_analysis(float **s,
                      uint32_t size,
                      double DC_bias,
                      double R_shunt,
                      float complex *Z,
                      double w_out,
                      int f) {
    int i2, i3;
    float **U_acq = create_2D_table_size(SIGNALS_NUM, SIGNAL_LENGTH);
    /* Used for storing the Voltage and current on the load */
    float *U_dut = create_table_size( SIGNAL_LENGTH );
    float *I_dut = create_table_size( SIGNAL_LENGTH );
    /* Signals multiplied by the reference signal (sin) */
    float *U_dut_sampled_X = (float *) malloc( size * sizeof( float ) );
    float *U_dut_sampled_Y = (float *) malloc( size * sizeof( float ) );
    float *I_dut_sampled_X = (float *) malloc( size * sizeof( float ) );
    float *I_dut_sampled_Y = (float *) malloc( size * sizeof( float ) );
    /* Signals return by trapezoidal method in complex */
    float *X_component_lock_in_1 = (float *) malloc( size * sizeof( float ) );
    float *X_component_lock_in_2 = (float *) malloc( size * sizeof( float ) );
    float *Y_component_lock_in_1 = (float *) malloc( size * sizeof( float ) );
    float *Y_component_lock_in_2 = (float *) malloc( size * sizeof( float ) );
    /* Voltage, current and their phases calculated */
    float U_dut_amp;
    float Phase_U_dut_amp;
    float I_dut_amp;
    float Phase_I_dut_amp;
    float Phase_Z_rad;
    float Z_amp;
    //float Z_phase_deg_imag;  // may cuse errors because not complex
    float T; // Sampling time in seconds
    float *t = create_table_size(16384);

    T = ( g_dec[ f ] / 125e6 );
    //printf("T = %f;\n",T );

    for( i2 = 0; i2 < ( size - 1 ); i2++ ) {
        t[ i2 ] = i2;
    }

    /* Transform signals from  AD - 14 bit to voltage [ ( s / 2^14 ) * 2 ] */
    for ( i2 = 0; i2 < SIGNALS_NUM; i2++) { // only the 1 and 2 are used for i2
        for( i3 = 0; i3 < size; i3 ++ ) { 
            //division comes after multiplication, this way no accuracy is lost
            U_acq[i2][i3] = ( ( s[ i2 ][ i3 ] ) * (float)( 2 - DC_bias ) ) / (float)16384 ; 
        }
    }

    /* Voltage and current on the load can be calculated from gathered data */
    for (i2 = 0; i2 < size; i2++) { 
        U_dut[ i2 ] = (U_acq[ 1 ][ i2 ] - U_acq[ 2 ][ i2 ]); // potencial difference gives the voltage
        // Curent trough the load is the same as trough thr R_shunt. ohm's law is used to calculate the current
        I_dut[ i2 ] = (U_acq[ 2 ][ i2 ] / R_shunt); 
    }

    /* Acquired signals must be multiplied by the reference signals, used for lock in metod */
    float ang;
    for( i2 = 0; i2 < size; i2++) {
        ang = (i2 * T * w_out);
        //printf("ang(%d) = %f \n", (i2+1), ang);
        U_dut_sampled_X[ i2 ] = U_dut[ i2 ] * sin( ang );
        U_dut_sampled_Y[ i2 ] = U_dut[ i2 ] * sin( ang+ ( M_PI/2 ) );

        I_dut_sampled_X[ i2 ] = I_dut[ i2 ] * sin( ang );
        I_dut_sampled_Y[ i2 ] = I_dut[ i2 ] * sin( ang +( M_PI/2 ) );
    }

    /* Trapezoidal method for calculating the approximation of an integral */
    X_component_lock_in_1[ 1 ] = trapz( U_dut_sampled_X, (float)T, size );
    Y_component_lock_in_1[ 1 ] = trapz( U_dut_sampled_Y, (float)T, size );

    X_component_lock_in_2[ 1 ] = trapz( I_dut_sampled_X, (float)T, size );
    Y_component_lock_in_2[ 1 ] = trapz( I_dut_sampled_Y, (float)T, size );
    
    /* Calculating voltage amplitude and phase */
    U_dut_amp = (float)2 * (sqrtf( powf( X_component_lock_in_1[ 1 ] , (float)2 ) + powf( Y_component_lock_in_1[ 1 ] , (float)2 )));
    Phase_U_dut_amp = atan2f( Y_component_lock_in_1[ 1 ], X_component_lock_in_1[ 1 ] );

    /* Calculating current amplitude and phase */
    I_dut_amp = (float)2 * (sqrtf( powf( X_component_lock_in_2[ 1 ], (float)2 ) + powf( Y_component_lock_in_2[ 1 ] , (float)2 ) ) );
    Phase_I_dut_amp = atan2f( Y_component_lock_in_2[1], X_component_lock_in_2[1] );
    
    /* Asigning impedance  values (complex value) */
    Phase_Z_rad =  Phase_U_dut_amp - Phase_I_dut_amp;
    Z_amp = U_dut_amp / I_dut_amp; // forming resistance


    /* Phase has to be limited between 180 and -180 deg. */
    if (Phase_Z_rad <=  (-M_PI) ) {
        Phase_Z_rad = Phase_Z_rad +(2*M_PI);
    }
    else if ( Phase_Z_rad >= M_PI ) {
        Phase_Z_rad = Phase_Z_rad -(2*M_PI) ;
    }
    else {
        Phase_Z_rad = Phase_Z_rad;
    } 
    
    *Z =  ( ( Z_amp ) * cosf( Phase_Z_rad ) )  +  ( ( Z_amp ) * sinf( Phase_Z_rad ) ) * I; // R + jX

    return 1;
}

/* user wait defined for user inquiry regarding measurement sweep
 * its functionality is not used and will be avaliable in the future if needed 
 * it lets the user know to connect the wires correctly and inquires for input
 */
int inquire_user_wait() {
    while(1) {
        int calibration_continue;
        printf("Please connect the wires correctly. Continue? [1 = yes|0 = skip ] :");
        if (fscanf(stdin, "%d", &calibration_continue) > 0) 
        {
            if(calibration_continue == 1)  {
                calibration_continue = 0;
                break;
            }
            else if(calibration_continue == 0)  {
                return -1;
            }
            else {} //ask again
            }
        else {
            printf("error when readnig from stdinput (scanf)\n");
            return -1;
        }
    }
    return 1;
}
