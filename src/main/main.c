#include <uquad_config.h>
#if DEBUG // The following define will affect includes
#define TIMING             0
#define TIMING_KALMAN      0
#define TIMING_IMU         0
#define TIMING_IO          0
#define LOG_ERR            1
#define LOG_W              1
#define LOG_W_CTRL         1
#define LOG_IMU_RAW        1
#define LOG_IMU_DATA       1
#define LOG_IMU_AVG        1
#define DEBUG_X_HAT        1
#define LOG_GPS            1
#define DEBUG_KALMAN_INPUT 1
#define LOG_TV             1
#define LOG_T_ERR          1
#define LOG_INT            (1 && CTRL_INTEGRAL)
#define LOG_BUKAKE         0
#endif

#if LOG_BUKAKE
#define LOG_BUKAKE_STDOUT  1
#endif

#include <manual_mode.h>
//#include <ncurses.h>
#include <stdio.h>
#include <uquad_error_codes.h>
#include <uquad_types.h>
#include <macros_misc.h>
#include <uquad_aux_io.h>
#include <imu_comm.h>
#include <mot_control.h>
#include <uquad_kalman.h>
#include <control.h>
#include <path_planner.h>
#include <uquad_logger.h>
#if USE_GPS
#include <uquad_gps_comm.h>
#endif

#include <sys/signal.h>   // for SIGINT and SIGQUIT
#include <unistd.h>       // for STDIN_FILENO

#define QUIT           27

#define UQUAD_HOW_TO   "./main <imu_device> /path/to/log/"
#define MAX_ERRORS     20
#define OL_TS_STABIL   0                 // If != 0, then will wait OL_TS_STABIL+STARTUP_RUNS samples before using IMU.
#define STARTUP_RUNS   (10+OL_TS_STABIL) // Wait for this number of samples at a steady Ts before running
#define STARTUP_KALMAN 100
#define FIXED          3
#define IMU_TS_OK      -1

#define LOG_DIR_DEFAULT    "/media/sda1/"

#define LOG_W_NAME         "w"
#define LOG_W_CTRL_NAME    "w_ctrl"

#define LOG_ERR_NAME       "err"
#define LOG_IMU_RAW_NAME   "imu_raw"
#define LOG_IMU_DATA_NAME  "imu_data"
#define LOG_IMU_AVG_NAME   "imu_avg"
#define LOG_X_HAT_NAME     "x_hat"
#define LOG_KALMAN_IN_NAME "kalman_in"
#define LOG_GPS_NAME       "gps"
#define LOG_TV_NAME        "tv"
#define LOG_T_ERR_NAME     "t_err"
#define LOG_INT_NAME       "int"
#define LOG_BUKAKE_NAME    "buk"

/**
 * Frequency at which motor controller is updated
 * Must be at least MOT_UPDATE_MAX_US
 *
 */
#define MOT_UPDATE_T MOT_UPDATE_MAX_US

/// Global structs
static imu_t *imu          = NULL;
static kalman_io_t *kalman = NULL;
static uquad_mot_t *mot    = NULL;
static io_t *io            = NULL;
static ctrl_t *ctrl        = NULL;
static path_planner_t *pp  = NULL;
#if USE_GPS && !GPS_ZERO
static gps_t *gps          = NULL;
#endif
/// Global var
uquad_mat_t *w = NULL, *wt = NULL, *w_ramp = NULL;
uquad_mat_t *x = NULL;
imu_data_t imu_data;
struct timeval tv_start = {0,0};
uquad_bool_t
/**
 * Flag to allow state estimation to keep running
 * after abort, without controlling motors.
 */
interrupted = false,
/**
 * Flag to indicate that main read/estimate/control loop
 * is running.
 */
running     = false;

#if USE_GPS
gps_comm_data_t *gps_dat;
#else
#define gps_dat NULL
#endif
/// Logs
#if DEBUG
#if LOG_ERR
FILE *log_err = NULL;
#endif // LOG_ERR
#if LOG_IMU_RAW
FILE *log_imu_raw;
#endif //LOG_IMU_RAW
#if LOG_IMU_DATA
FILE *log_imu_data;
#endif //LOG_IMU_DATA
#if LOG_IMU_AVG
FILE *log_imu_avg;
#endif //LOG_IMU_AVG
#if LOG_W
FILE *log_w = NULL;
#endif //LOG_W
#if LOG_W
FILE *log_w_ctrl = NULL;
#endif //LOG_W
#if DEBUG_X_HAT
FILE *log_x_hat = NULL;
uquad_mat_t *x_hat_T = NULL;
#endif //DEBUG_X_HAT
#if DEBUG_KALMAN_INPUT
FILE *log_kalman_in = NULL;
#endif //DEBUG_KALMAN_INPUT
#if LOG_GPS && USE_GPS
FILE *log_gps = NULL;
#endif // LOG_GPS && USE_GPS
#if LOG_BUKAKE && !LOG_BUKAKE_STDOUT
FILE *log_bukake = NULL;
#endif //LOG_BUKAKE
#if LOG_TV
FILE *log_tv = NULL;
#endif // LOG_TV
#if LOG_T_ERR
FILE *log_t_err = NULL;
#endif // LOG_T_ERR
#if LOG_INT
FILE *log_int = NULL;
#endif // LOG_INT
#endif //DEBUG

/**
 * Will print configuration to err log.
 *
 */
/* void log_config(void) */
/* { */

/* } */

/** 
 * Clean up and close
 * 
 */
void quit()
{
    int retval;
    struct timeval
	tv_tmp,
	tv_diff;
    if(!interrupted)
    {
	/**
	 * Kill motors, keep IMU+kalman running to log data
	 *
	 */
	retval = mot_deinit(mot);
	if(retval != ERROR_OK)
	{
	    err_log("Could not close motor driver correctly!");
	}
	interrupted = true;
	if(running)
	{
	    // Let IMU gather data for a while
	    gettimeofday(&tv_tmp, NULL);
	    retval = uquad_timeval_substract(&tv_diff,tv_tmp,tv_start);
	    if(retval > 0)
	    {
		err_log_tv("Motors killed!",tv_diff);
	    }
	    return;
	}
    }
    /* clear(); */
    /* endwin(); */
    gettimeofday(&tv_tmp, NULL);
    retval = uquad_timeval_substract(&tv_diff,tv_tmp,tv_start);
    if(retval > 0)
    {
	err_log_tv("main deinit started...",tv_diff);
    }

    /// IO manager
    retval = io_deinit(io);
    if(retval != ERROR_OK)
    {
	err_log("Could not close IO correctly!");
    }

    /// IMU
    retval = imu_comm_deinit(imu);
    if(retval != ERROR_OK)
    {
	err_log("Could not close IMU correctly!");
    }

    /// Kalman
    kalman_deinit(kalman);

    /// Control module
    control_deinit(ctrl);

    /// Path planner module
    pp_deinit(pp);

    /// Global vars
    uquad_mat_free(w);
    uquad_mat_free(w_ramp);
    uquad_mat_free(wt);
    uquad_mat_free(x);
    uquad_mat_free(imu_data.acc);
    uquad_mat_free(imu_data.gyro);
    uquad_mat_free(imu_data.magn);
#if USE_GPS
    gps_comm_data_free(gps_dat);
#if !GPS_ZERO
    gps_comm_deinit(gps);
#endif // !GPS_ZERO
#endif

    // Logs
#if DEBUG
#if LOG_ERR
    uquad_logger_remove(log_err);
#endif // LOG_ERR
#if LOG_IMU_RAW
    uquad_logger_remove(log_imu_raw);
#endif //LOG_IMU_RAW
#if LOG_IMU_DATA
    uquad_logger_remove(log_imu_data);
#endif //LOG_IMU_DATA
#if LOG_IMU_AVG
    uquad_logger_remove(log_imu_avg);
#endif //LOG_IMU_AVG
#if LOG_W
    uquad_logger_remove(log_w);
#endif //LOG_W
#if LOG_W_CTRL
    uquad_logger_remove(log_w_ctrl);
#endif //LOG_W_CTRL
#if DEBUG_X_HAT
    uquad_logger_remove(log_x_hat);
    uquad_mat_free(x_hat_T);
#endif //DEBUG_X_HAT
#if DEBUG_KALMAN_INPUT
    uquad_logger_remove(log_kalman_in);
#endif
#if LOG_GPS && USE_GPS
    uquad_logger_remove(log_gps);
#endif //LOG_GPS && USE_GPS
#if LOG_BUKAKE
#if !LOG_BUKAKE_STDOUT
    uquad_logger_remove(log_bukake);
#else
#define log_bukake stdout
#endif // !LOG_BUKAKE_STDOUT
#endif // LOG_BUKAKE
#if LOG_TV
    uquad_logger_remove(log_tv);
#endif // LOG_TV
#if LOG_T_ERR
    uquad_logger_remove(log_t_err);
#endif // LOG_T_ERR
#if LOG_INT
    uquad_logger_remove(log_int);
#endif // LOG_INT
#endif //DEBUG

    //TODO deinit everything?
    exit(retval);
}

/**
 * Save configuration to log file.
 *
 */
void log_configuration(void)
{
    err_log_eol();
    err_log("-- -- -- -- -- -- -- --");
    err_log("main.c configuration:");
    err_log("-- -- -- -- -- -- -- --");
    err_log_num("DEBUG",DEBUG);
    err_log_num("KALMAN_BIAS",KALMAN_BIAS);
    err_log_num("CTRL_INTEGRAL",CTRL_INTEGRAL);
    err_log_num("FULL_CONTROL",FULL_CONTROL);
    err_log_num("USE_GPS",USE_GPS);
    err_log_num("GPS_ZERO",GPS_ZERO);
    err_log_num("IMU_COMM_FAKE",IMU_COMM_FAKE);
    err_log_num("OL_TS_STABIL",OL_TS_STABIL);
    err_log_double("MASA_DEFAULT",MASA_DEFAULT);
    err_log("-- -- -- -- -- -- -- --");
    err_log_eol();
}

void uquad_sig_handler(int signal_num)
{
    err_log_num("Caught signal:",signal_num);
    quit();
}

int main(int argc, char *argv[]){
    int
	retval = ERROR_OK,
	i,
	input,
	imu_ts_ok   = 0,
	runs_imu    = 0,
	runs_kalman = 0,
	err_imu     = ERROR_OK,
	err_gps     = ERROR_OK;
    char
	*device_imu,
	*device_gps,
	*log_path;
    double
	dtmp;
    unsigned long
	kalman_loops = 0,
	ts_error_wait = 0;

    uquad_bool_t
	read        = false,
	write       = false,
	imu_update  = false,
	reg_stdin   = true,
	manual_mode = false;
    struct timeval
	tv_tmp, tv_diff,
	tv_last_m_cmd,
	tv_timing_off,
	tv_last_ramp,
	tv_last_kalman,
	tv_last_imu,
	tv_last_frame,
	tv_gps_last;
#if IMU_COMM_FAKE
    struct timeval tv_imu_fake;
#endif // IMU_COMM_FAKE

    // Catch signals
    signal(SIGINT, uquad_sig_handler);
    signal(SIGQUIT, uquad_sig_handler);

    retval = gettimeofday(&tv_start,NULL);
    err_log_std(retval);
    tv_last_ramp  = tv_start;
    tv_last_m_cmd = tv_start;
    uquad_bool_t gps_update = false;
#if USE_GPS
    tv_gps_last   = tv_start;
#if !GPS_ZERO
    uquad_bool_t reg_gps = true;
#endif // !GPS_ZERO
#endif // USE_GPS
#if TIMING
    struct timeval
	tv_pgm,
	tv_last_io_ok;
    gettimeofday(&tv_last_io_ok,NULL);
    gettimeofday(&tv_pgm,NULL);
#endif
#if TIMING && TIMING_IMU
    struct timeval tv_last_imu_read, tv_imu_start, tv_imu_diff;
    gettimeofday(&tv_last_imu_read,NULL);
#endif
    int count_err = 0, count_ok = FIXED;

#if LOG_IMU_RAW || LOG_IMU_DATA
    imu_raw_t imu_frame;
#endif // LOG_IMU_RAW || LOG_IMU_DATA

    /**
     * Init curses library, used for user input
     */
    /* initscr();  // init curses lib */
    /* cbreak();   // get user input without waiting for RET */
    /* noecho();   // do no echo user input on screen */
    /* timeout(0); // non-blocking reading of user input */
    /* refresh();  // show output on screen */

    if(argc<2)
    {
	err_log(UQUAD_HOW_TO);
	exit(1);
    }
    else
    {
	device_imu = argv[1];
	if(argc < 3)
	    log_path = LOG_DIR_DEFAULT;
	else
	    log_path   = argv[2];

	if(argc < 4)
	    device_gps = NULL;
	else
	    device_gps = argv[3];
    }

    /// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
    /// Init
    /// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

    /// Logs
#if DEBUG
#if LOG_ERR
    log_err = uquad_logger_add(LOG_ERR_NAME, log_path);
    if(log_err == NULL)
    {
	err_log("Failed to open log_imu_raw!");
	quit();
    }
    /**
     * Re-route stderr to log file.
     * This is required, since errors in uquad_error_codes.h use macros
     * that log to stderr. We want errors in a log file, so in order to
     * get every message from every module, to the log we need to re-define stderr.
     */
    stderr = log_err;
#endif // LOG_ERR
#if LOG_IMU_RAW
    log_imu_raw = uquad_logger_add(LOG_IMU_RAW_NAME, log_path);
    if(log_imu_raw == NULL)
    {
	err_log("Failed to open log_imu_raw!");
	quit();
    }
#endif //LOG_IMU_RAW
#if LOG_IMU_DATA
    log_imu_data = uquad_logger_add(LOG_IMU_DATA_NAME, log_path);
    if(log_imu_data == NULL)
    {
	err_log("Failed to open log_imu_data!");
	quit();
    }
#endif //LOG_IMU_DATA
#if LOG_IMU_AVG
    log_imu_avg = uquad_logger_add(LOG_IMU_AVG_NAME, log_path);
    if(log_imu_avg == NULL)
    {
	err_log("Failed to open log_imu_avg!");
	quit();
    }
#endif //LOG_IMU_AVG
#if LOG_W
    log_w = uquad_logger_add(LOG_W_NAME, log_path);
    if(log_w == NULL)
    {
	err_log("Failed to open log_w!");
	quit();
    }
#endif //LOG_W
#if LOG_W_CTRL
    log_w_ctrl = uquad_logger_add(LOG_W_CTRL_NAME, log_path);
    if(log_w_ctrl == NULL)
    {
	err_log("Failed to open log_w_ctrl!");
	quit();
    }
#endif //LOG_W_CTRL
#if DEBUG_X_HAT
    log_x_hat = uquad_logger_add(LOG_X_HAT_NAME, log_path);
    if(log_x_hat == NULL)
    {
	err_log("Failed to open x_hat!");
	quit();
    }
#endif //DEBUG_X_HAT
#if DEBUG_KALMAN_INPUT
    log_kalman_in = uquad_logger_add(LOG_KALMAN_IN_NAME, log_path);
    if(log_kalman_in == NULL)
    {
	err_log("Failed open kalman_in log!");
	quit();
    }
#endif
#if LOG_GPS && USE_GPS
    log_gps = uquad_logger_add(LOG_GPS_NAME, log_path);
    if(log_gps == NULL)
    {
	err_log("Failed open kalman_in log!");
	quit();
    }
#endif //LOG_GPS && USE_GPS
#if LOG_BUKAKE && !LOG_BUKAKE_STDOUT
    log_bukake = uquad_logger_add(LOG_BUKAKE_NAME, log_path);
    if(log_bukake == NULL)
    {
	err_log("Failed open kalman_in log!");
	quit();
    }
#endif //LOG_BUKAKE && !LOG_BUKAKE_STDOUT
#if LOG_TV
    log_tv = uquad_logger_add(LOG_TV_NAME, log_path);
    if(log_tv == NULL)
    {
	err_log("Failed to open tv_log!");
	quit();
    }
#endif // LOG_TV
#if LOG_T_ERR
    log_t_err = uquad_logger_add(LOG_T_ERR_NAME, log_path);
    if(log_t_err == NULL)
    {
	err_log("Failed to open t_err_log!");
	quit();
    }
#endif // LOG_T_ERROR
#if LOG_INT
    log_int = uquad_logger_add(LOG_INT_NAME, log_path);
    if(log_int == NULL)
    {
	err_log("Failed to open t_err_log!");
	quit();
    }
#endif // LOG_INT
#endif //DEBUG

    /// IO manager
    io = io_init();
    if(io==NULL)
    {
	quit_log_if(ERROR_FAIL,"io init failed!");
    }

    /// IMU
    imu = imu_comm_init(device_imu);
    if(imu == NULL)
    {
	quit_log_if(ERROR_FAIL,"imu init failed!");
    }

#if USE_GPS
    gps_dat = gps_comm_data_alloc();
    if(gps_dat == NULL)
    {
	err_log("Failed to allocate GPS!...");
	quit();
    }
#if !GPS_ZERO
    /// GPS
    gps = gps_comm_init(device_gps);
    if(gps == NULL)
    {
	quit_log_if(ERROR_FAIL,"gps init failed!");
    }
    else
    {
	uquad_bool_t got_fix;
	struct timeval tv_gps_init_t_out;
	tv_gps_init_t_out.tv_sec = GPS_INIT_TOUT_S;
	tv_gps_init_t_out.tv_usec = GPS_INIT_TOUT_US;
	if(device_gps != NULL)
	{
	    retval = gps_comm_read(gps, &got_fix, &tv_start);
	    quit_if(retval);
	    if(!got_fix)
		quit_log_if(ERROR_READ, "Failed to read from log file!");
	}
	else
	{
	    err_log("Waiting for GPS fix...");
	    retval = gps_comm_wait_fix(gps,&got_fix,&tv_gps_init_t_out);
	    quit_if(retval);
	    if(!got_fix)
	    {
		quit_log_if(ERROR_GPS,"Failed to get GPS fix!");
	    }
	    err_log("GPS fix ok.");
	}

	/**
	 * Now get initial position from GPS.
	 * This information will be used as startpoint for the kalman
	 * estimator if no other GPS updates are received during IMU
	 * warmup.
	 */
	retval = gps_comm_get_data_unread(gps, gps_dat, NULL);
	quit_log_if(retval,"Failed to get initial position from GPS!");
	retval = gps_comm_set_0(gps,gps_dat);
	quit_if(retval);
	retval = gps_comm_get_0(gps, gps_dat);
	quit_if(retval);

	/**
	 * Inform IMU about starting altitude, in order to allow barometer
	 * data to match GPS altitud estimation
	 *
	 */
	if(imu == NULL)
	{
	    quit_log_if(ERROR_FAIL,"IMU must be initialized before gps!");
	}
	retval = imu_comm_set_z0(imu,gps_dat->pos->m_full[2]);
	quit_if(retval);
    }
#endif // !GPS_ZERO
#endif // USE_GPS

    /// Kalman
    kalman = kalman_init();
    if(kalman == NULL)
    {
	quit_log_if(ERROR_FAIL,"kalman init failed!");
    }

    /// Motors
    mot = mot_init();
    if(mot == NULL)
    {
	quit_log_if(ERROR_FAIL,"mot init failed!");
    }

    /// Control module
    ctrl = control_init();
    if(ctrl == NULL)
    {
	quit_log_if(ERROR_FAIL,"control init failed!");
    }

    /// Path planner module
    pp = pp_init();
    if(pp == NULL)
    {
	quit_log_if(ERROR_FAIL,"path planner init failed!");
    }

    /// Global vars
    w = uquad_mat_alloc(4,1);        // Current angular speed [rad/s]
    wt = uquad_mat_alloc(1,4);        // tranpose(w)
    w_ramp = uquad_mat_alloc(4,1);
    x = uquad_mat_alloc(1,STATE_COUNT);   // State vector
    retval = imu_data_alloc(&imu_data);
    quit_if(retval);

    if( x == NULL || w == NULL || wt == NULL || w_ramp == NULL)
    {
	err_log("Cannot run without x or w, aborting...");
	quit();
    }

#if DEBUG_X_HAT
    x_hat_T = uquad_mat_alloc(1,STATE_COUNT+STATE_BIAS);
    if(x_hat_T == NULL)
    {
	err_log("Failed alloc x_hat_T!");
	quit();
    }
#endif // DEBUG_X_HAT

    /**
     * Save configuration to log file
     *
     */
    retval = kalman_dump(kalman, log_err);
    quit_log_if(retval,"Failed to save Kalman configuration!");
    log_configuration();

    /// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
    /// Register IO devices
    /// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
    // imu
    int imu_fds;
    retval = imu_comm_get_fds(imu,& imu_fds);
    quit_log_if(retval,"Failed to get imu fds!!");
    retval = io_add_dev(io,imu_fds);
    quit_log_if(retval,"Failed to add imu to dev list");
#if USE_GPS && !GPS_ZERO
    // gps
    int gps_fds;
    if(gps != NULL)
    {
	gps_fds = gps_comm_get_fd(gps);
	retval = io_add_dev(io,gps_fds);
	quit_log_if(retval,"Failed to add gps to dev list");
    }
#endif
    // stdin
    retval = io_add_dev(io,STDIN_FILENO);
    quit_log_if(retval, "Failed to add stdin to io list");

    /// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
    /// Startup your engines...
    /// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
    retval = mot_set_idle(mot);
    quit_log_if(retval, "Failed to startup motors!");

    /// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
    /// Poll n read loop
    /// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
    retval = ERROR_OK;
    //    poll_n_read:
    gettimeofday(&tv_tmp,NULL);
    uquad_timeval_substract(&tv_diff,tv_tmp,tv_start);
    err_log_tv("Entering while:",tv_diff);
    running = true;
    while(1)
    {
	fflush(stdout); // flushes output, but does not display on screen
	//	refresh();      // displays flushed output on screen
	if((runs_imu == IMU_TS_OK) &&
	   (retval != ERROR_OK  ||
	    err_imu != ERROR_OK ||
	    err_gps != ERROR_OK))
	{
	    count_ok = 0;
	    if(count_err++ > MAX_ERRORS)
	    {
		gettimeofday(&tv_tmp,NULL);
		retval = uquad_timeval_substract(&tv_diff, tv_tmp, tv_start);
		err_log_tv("Too many errors! Aborting...",tv_diff);
		quit();
		/// program ends here
	    }
	}
	else
	{
	    if(count_ok < FIXED)
	    {
		count_ok++;
	    }
	    else if(count_err > 0)
	    {
		// forget abour error
		err_log_num("Recovered! Errors:",count_err);
		count_err = 0;
	    }
	}
	// reset error/update indicators
	imu_update = false;
	err_imu = ERROR_OK;
	//gps_update = false; // This is cleared within the loop
	err_gps = ERROR_OK;
	retval = ERROR_OK;

	retval = io_poll(io);
	quit_log_if(retval,"io_poll() error");

	/// -- -- -- -- -- -- -- --
	/// Check IMU updates
	/// -- -- -- -- -- -- -- --
	retval = io_dev_ready(io,imu_fds,&read,&write);
	quit_log_if(retval,"io_dev_ready() error");
	if(read)
	{
#if TIMING && TIMING_IO
	    err_imu = gettimeofday(&tv_tmp,NULL);
	    err_log_std(err_imu);
	    err_imu = uquad_timeval_substract(&tv_diff,tv_tmp,tv_last_io_ok);
	    if(err_imu < 0)
	    {
		err_log("Timing error!");
	    }
	    err_imu = gettimeofday(&tv_last_io_ok,NULL);
	    err_log_std(err_imu);
	    err_imu = gettimeofday(&tv_pgm,NULL);
	    err_log_std(err_imu);
	    printf("IO:\t%ld\t\t%ld.%06ld\n", tv_diff.tv_usec,
		   tv_pgm.tv_sec - tv_start.tv_sec,
		   tv_pgm.tv_usec);
#endif
#if TIMING && TIMING_IMU
	    err_imu = gettimeofday(&tv_imu_start,NULL);
	    err_log_std(err_imu);
#endif

	    err_imu = imu_comm_read(imu, &imu_update);
	    log_n_jump(err_imu,end_imu,"imu_comm_read() failed!");
	    if(!imu_update)
	    {
		goto end_imu;
	    }
	    imu_update = false; // data may not be of direct use, may be calib


	    err_imu = imu_comm_get_raw_latest(imu,&imu_frame);
	    log_n_jump(err_imu,end_imu,"could not get new frame...");

#if IMU_COMM_FAKE
	    // simulate delay (no delay when reading from txt)
	    if(runs_imu == 0)
	    {
		tv_diff = imu_frame.timestamp;
	    }
	    else
	    {
		err_imu = uquad_timeval_substract(&tv_diff, imu_frame.timestamp, tv_imu_fake);
		if(err_imu < 0)
		{
		    err_log("Absurd fake IMU timing!");
		}
	    }
	    tv_imu_fake = imu_frame.timestamp;
	    if(tv_diff.tv_sec > 0)
		sleep(tv_diff.tv_sec);
	    usleep(tv_diff.tv_usec);
#endif // IMU_COMM_FAKE

	    err_imu = gettimeofday(&tv_tmp,NULL);
	    err_log_std(err_imu);
#if LOG_IMU_RAW || LOG_IMU_DATA
	    err_imu = uquad_timeval_substract(&tv_diff,tv_tmp,tv_start);
	    if(err_imu < 0)
	    {
		err_log("Timing error!");
	    }
#if LOG_IMU_RAW
	    log_tv_only(log_imu_raw,tv_diff);
	    err_imu= imu_comm_print_raw(&imu_frame, log_imu_raw);
	    log_n_jump(err_imu,end_imu,"could not print new raw frame...");
	    fflush(log_imu_raw);
#endif // LOG_IMU_RAW
#if LOG_IMU_DATA
	    err_imu = imu_comm_raw2data(imu, &imu_frame, &imu_data);
	    log_n_jump(err_imu,end_imu,"could not convert new raw...");
	    log_tv_only(log_imu_data,tv_diff);
	    err_imu = imu_comm_print_data(&imu_data, log_imu_data);
	    log_n_jump(err_imu,end_imu,"could not print new data...");
	    fflush(log_imu_data);
#endif // LOG_IMU_DATA
#endif // LOG_IMU_RAW || LOG_IMU_DATA

#if TIMING && TIMING_IMU
	    err_imu = gettimeofday(&tv_tmp,NULL);
	    err_log_std(err_imu);
	    err_imu = uquad_timeval_substract(&tv_diff,tv_tmp,tv_last_imu_read);
	    if(err_imu < 0)
	    {
		err_log("Timing error!");
	    }
	    err_imu = uquad_timeval_substract(&tv_imu_diff,tv_tmp,tv_imu_start);
	    if(err_imu < 0)
	    {
		err_log("Timing error!");
	    }
	    err_imu = gettimeofday(&tv_last_imu_read,NULL);
	    err_log_std(err_imu);
	    err_imu = gettimeofday(&tv_pgm,NULL);
	    err_log_std(err_imu);
	    printf("IMU:\t%ld\tDELAY:\t%ld\t\t%ld.%06ld\n",
		   tv_diff.tv_usec, tv_imu_diff.tv_usec,
		   tv_pgm.tv_sec - tv_start.tv_sec,
		   tv_pgm.tv_usec);
#endif

	    /// discard first samples
	    if(runs_imu >= 0)
	    {
		if(runs_imu == 0)
		{
		    err_log("Waiting for stable IMU sampling time...");
		    tv_last_frame = tv_start;
		}
		runs_imu++;
		err_imu = gettimeofday(&tv_tmp, NULL);
		err_log_std(err_imu);
		err_imu = uquad_timeval_substract(&tv_diff, tv_tmp, tv_last_frame);
		log_n_jump((err_imu < 0)?ERROR_TIMING:ERROR_OK,end_imu,"Absurd timing!");
		err_imu = in_range_us(tv_diff, TS_MIN, TS_MAX);
		if(err_imu == 0 || OL_TS_STABIL)
		{
		    if(imu_ts_ok++ >= STARTUP_RUNS)
		    {
			err_log_num("IMU: Frames read out during stabilization:",runs_imu);
			runs_imu = IMU_TS_OK; // so re-entry doesn't happen
			err_imu = gettimeofday(&tv_last_imu,NULL);
			err_log_std(err_imu);
			tv_gps_last    = tv_last_imu;
			tv_last_kalman = tv_last_imu;
			err_imu = uquad_timeval_substract(&tv_diff,tv_last_imu,tv_start);
			if(err_imu < 0)
			{
			    log_n_jump(err_imu,end_imu,"Absurd IMU startup time!");
			}
			err_imu = ERROR_OK; // clear timing info
			err_log_tv("IMU startup completed, starting calibration...", tv_diff);
		    }
		}
		else
		{
		    // We want consecutive stable samples
		    imu_ts_ok = 0;
		}
		tv_last_frame = tv_tmp;
		goto end_imu;
	    }

	    /// check calibration status
	    if(imu_comm_get_status(imu) == IMU_COMM_STATE_CALIBRATING)
		// if calibrating, then data should not be used.
		goto end_imu;
	    else if(!imu_comm_calib_estim(imu))
	    {
		// if no calibration estim exists, build one.
		err_imu = imu_comm_calibration_start(imu);
		log_n_jump(err_imu,end_imu,"Failed to start calibration!");
		goto end_imu;
	    }

	    /// Get new unread data
	    if(!imu_comm_unread(imu) || !imu_comm_avg_ready(imu))
	    {
		// we only used averaged data
		goto end_imu;
	    }

	    gettimeofday(&tv_tmp,NULL);

	    err_imu = imu_comm_get_avg_unread(imu,&imu_data);
	    log_n_jump(err_imu,end_imu,"IMU did not have new avg!");
#if LOG_IMU_AVG
	    uquad_timeval_substract(&tv_diff, tv_tmp, tv_start);
	    log_tv_only(log_imu_avg, tv_diff);
	    err_imu = imu_comm_print_data(&imu_data, log_imu_avg);
	    log_n_jump(err_imu,end_imu,"Failed to log imu avg!");
#endif // LOG_IMU_AVG

	    err_imu = uquad_timeval_substract(&tv_diff,tv_tmp,tv_last_imu);
	    if(err_imu < 0)
	    {
		log_n_jump(ERROR_TIMING,end_imu,"Timing error!");
	    }

	    // store time since last IMU sample
	    imu_data.timestamp = tv_diff;

	    /// new data will be useful!
	    imu_update = true;
	    err_imu = gettimeofday(&tv_last_imu,NULL);
	    err_log_std(err_imu);

	    end_imu:;
	    // will jump here if something went wrong during IMU reading
	}//if(read)

#if USE_GPS
#if !GPS_ZERO
	/// -- -- -- -- -- -- -- --
	/// Check GPS updates
	/// -- -- -- -- -- -- -- --
	if(reg_gps && !gps_update)
	{
	    err_gps = io_dev_ready(io,gps_fds,&read,&write);
	    if(read)
	    {
		gettimeofday(&tv_tmp,NULL); // Will be used in log_gps
		if(device_gps != NULL)
		    err_gps = gps_comm_read(gps,&gps_update,&tv_tmp);
		else
		    err_gps = gps_comm_read(gps,&gps_update,NULL);
		log_n_jump(err_gps,end_gps,"GPS had no data!");
		if((!gps_update && (device_gps != NULL)) ||
		   (runs_kalman < 1) || !gps_comm_3dfix(gps))
		    // ignore startup data
		    goto end_gps;

		// Use latest IMU update to estimate speed from GPS data
		err_gps = gps_comm_get_data_unread(gps, gps_dat, NULL);
		log_n_jump(err_gps,end_gps,"Failed to get GPS data!");
		
		err_gps = uquad_timeval_substract(&tv_diff,tv_tmp,tv_start);
		if(err_gps < 0)
		{
		    log_n_jump(err_gps,end_gps,"Absurd GPS timing!");
		}
		err_gps = ERROR_OK; // clear error
#if LOG_GPS
		log_tv_only(log_gps, tv_diff);
		gps_comm_dump(gps, gps_dat, log_gps);
#endif // LOG_GPS
		tv_gps_last = tv_tmp;
	    }
	    end_gps:;
	    // will jump here if something went wrong during GPS reading
	}
#else // GPS_ZERO
	if(!gps_update)
	{
	    gettimeofday(&tv_tmp,NULL);
	    retval = uquad_timeval_substract(&tv_diff, tv_tmp, tv_gps_last);
	    if( (runs_kalman > 0) && (tv_diff.tv_sec > 0) )
	    {
		// gps_dat is set to 0 when allocated, so just use it.
		gps_update = true;
		tv_gps_last = tv_tmp;
		if(pp->pt != HOVER)
		{
		    quit_log_if(ERROR_GPS, "Fake GPS does not make sense if not hovering!");
		}
	    }
	    else
	    {
		gps_update = false;
	    }
	    retval = ERROR_OK; // clear retval
	}
#endif // GPS_ZERO
#endif // USE_GPS

	/// -- -- -- -- -- -- -- --
	/// check if new data
	/// -- -- -- -- -- -- -- --
	if(!imu_update || interrupted)
	    /**
	     * We don't check gps_update here.
	     * If gps_update && !imu_update, then
	     * wait until imu_update to avoid unstable T_s.
	     *   T_gps = 1s
	     *   T_imu = 10ms
	     * so use approx. that T_gps+T_imu ~ T_gps.
	     *
	     * interrupted: If main was interrupted, we want to
	     * log data but the motors should not be controlled any more.
	     */
	    continue;
	/// -- -- -- -- -- -- -- --
	/// Startup Kalman estimator
	/// -- -- -- -- -- -- -- --
	if(runs_kalman == 0)
	{
	    /**
	     * Use current IMU calibration to set
	     * yaw == 0, this will keep us looking forward
	     * when hovering.
	     */
	    if(pp->pt != HOVER)
	    {
		quit_log_if(retval,"ERR: theta (Yaw) set to IMU calibration"\
			    "is only valid when in HOVER mode");
	    }
	    gettimeofday(&tv_tmp,NULL); // Will be used later
	    retval = uquad_timeval_substract(&tv_diff,tv_tmp,tv_start);
	    err_log_tv((retval < 0)?"Absurd IMU calibration time!":
		       "IMU calibration completed, running kalman+control+ramp",
		       tv_diff);
	    retval = imu_comm_raw2data(imu, &imu->calib.null_est, &imu_data);
	    quit_log_if(retval,"Failed to correct setpoint!");

#if USE_GPS && !GPS_ZERO
	    if(device_gps != NULL)
	    {
		/**
		 * If reading from gps log file, then change timestamp to avoid
		 * all draining the log because of the time spent in startup+calibration
		 *
		 */
		retval = gps_comm_set_tv_start(gps,tv_tmp);
		quit_log_if(retval, "Failed to set gps startup time!");
	    }
#endif // USE_GPS && !GPS_ZERO

	    /**
	     * Startup:
	     *  - Kalman estimator from calibration & GPS, if available.
	     *  - If hovering, set initial position as setpoint. This will avoid
	     *    rough movements on startup (setpoint will match current state).
	     */
	    if(pp->pt == HOVER)
	    {
#if USE_GPS
		// Position
		retval = uquad_mat_set_subm(pp->sp->x,SV_X,0,gps_dat->pos);
		quit_log_if(retval, "Failed to initiate kalman pos estimator from GPS data!");
#endif // USE_GPS
		// Euler angles
		pp->sp->x->m_full[SV_THETA] = imu_data.magn->m_full[2];
		pp->sp->x->m_full[SV_PSI]   = 0.0; // [rad]
		// Motor speed
		for(i=0; i<MOT_C; ++i)
		    w_ramp->m_full[i] = mot->w_min;
	    }

	    /**
	     * Startup Kalman estimator
	     *
	     */
#if USE_GPS
	    // Position
	    retval = uquad_mat_set_subm(kalman->x_hat,SV_X,0,gps_dat->pos);
	    quit_log_if(retval, "Failed to initiate kalman pos estimator from GPS data!");
	    // Velocity
	    //	    retval = uquad_mat_set_subm(kalman->x_hat,SV_VQX,0,gps_dat->pos);
	    //	    quit_log_if(retval, "Failed to initiate kalman vel estimator from GPS data!");
#endif // USE_GPS
	    // Euler angles
	    pp->sp->x->m_full[SV_Z] = 0;
	    kalman->x_hat->m_full[SV_PSI]   = imu_data.magn->m_full[0];
	    kalman->x_hat->m_full[SV_PHI]   = imu_data.magn->m_full[1];
	    kalman->x_hat->m_full[SV_THETA] = imu_data.magn->m_full[2];
#if KALMAN_BIAS
	    kalman->x_hat->m_full[SV_BAX] = imu_data.acc->m_full[0];
	    kalman->x_hat->m_full[SV_BAY] = imu_data.acc->m_full[1];
	    kalman->x_hat->m_full[SV_BAZ] = imu_data.acc->m_full[2] - GRAVITY;
#endif
	    retval = imu_comm_print_data(&imu_data, stderr);
	    if(retval != ERROR_OK)
	    {
		err_log("Failed to print IMU calibration!");
	    }
	    retval = ERROR_OK;// ignore error
	    fflush(stderr);
	}

	/// -- -- -- -- -- -- -- --
	/// Update state estimation
	/// -- -- -- -- -- -- -- --
	gettimeofday(&tv_tmp,NULL); // will be used to set tv_last_kalman
	if(runs_kalman == 0)
	{
	    // First time here, use fake timestamp
	    tv_diff.tv_usec = TS_DEFAULT_US;
	}
	else
	{
	    retval = uquad_timeval_substract(&tv_diff,tv_tmp,tv_last_kalman);
	    if(retval < 0)
	    {
		log_n_continue(ERROR_TIMING,"Absurd timing!");
	    }
#if TIMING && TIMING_KALMAN
	    gettimeofday(&tv_pgm,NULL);
	    printf("KALMAN:\t%ld\t\t%ld.%06ld\n", tv_diff.tv_usec,
		   tv_pgm.tv_sec - tv_start.tv_sec,
		   tv_pgm.tv_usec);
#endif
	    /// Check sampling period jitter
	    retval = in_range_us(tv_diff, TS_MIN, TS_MAX);
	    kalman_loops = (kalman_loops+1)%32768;// avoid overflow
	    if(retval != 0)
	    {
#if LOG_T_ERR
		uquad_timeval_substract(&tv_timing_off, tv_tmp, tv_start);
		log_eol(log_t_err);
		log_tv_only(log_t_err,tv_timing_off);
		log_tv_only(log_t_err,tv_diff);
		fflush(log_t_err);
#endif // LOG_T_ERR
		if(ts_error_wait == 0)
		{
		    // Avoid saturating log
		    err_log_tv("TS supplied to Kalman out of range!:",tv_diff);
		    ts_error_wait = TS_ERROR_WAIT;
		}
		ts_error_wait--;
		/// Lie to kalman, avoid large drifts
		tv_diff.tv_usec = (retval > 0) ? TS_MAX:TS_MIN;
	    }
	    else
	    {
		// Print next T_s error immediately
		ts_error_wait = 0;
	    }
	}
	// use real w
	retval = uquad_kalman(kalman,
			      (runs_kalman > STARTUP_KALMAN)?
			      mot->w_curr:w_ramp,
			      &imu_data,
			      tv_diff.tv_usec,
			      mot->weight,
			      gps_update?gps_dat:NULL);
	log_n_continue(retval,"Inertial Kalman update failed");

	/// Mark time when we run Kalman
	tv_last_kalman = tv_tmp;
#if USE_GPS
	if(gps_update)
	{
	    gettimeofday(&tv_tmp,NULL);
	    retval = uquad_timeval_substract(&tv_diff, tv_tmp, tv_start);
	    retval = ERROR_OK; // clear to avoid errors
	    gps_update = false; // Clear gps status
	    log_tv_only(log_gps, tv_diff);
	    log_eol(log_gps);
	}
#endif // USE_GPS


#if DEBUG
#if DEBUG_KALMAN_INPUT
	uquad_timeval_substract(&tv_diff,tv_last_kalman,tv_start);
	log_tv_only(log_kalman_in,tv_diff);
	retval = imu_comm_print_data(&imu_data, log_kalman_in);
	fflush(log_kalman_in);
#endif //DEBUG_KALMAN_INPUT
#if DEBUG_X_HAT
	retval = uquad_mat_transpose(x_hat_T, kalman->x_hat);
	quit_if(retval);
	uquad_mat_dump(x_hat_T,log_x_hat);
	fflush(log_x_hat);
#endif //DEBUG_X_HAT
#endif //DEBUG
	if(!(runs_kalman > STARTUP_KALMAN))
	{
	    /**
	     * Startup:
	     *   - Ramp motors.
	     */
	    ++runs_kalman;
	    if(runs_kalman == STARTUP_KALMAN)
	    {
		gettimeofday(&tv_tmp,NULL);
		retval = uquad_timeval_substract(&tv_diff,tv_tmp,tv_start);
		if(retval < 0)
		{
		    err_log("Absurd Kalman startup time!");
		    continue;
		}
		retval = ERROR_OK;
		// save to error log
		err_log("-- --");
		err_log("-- -- -- -- -- -- -- --");
		err_log_tv("Ramp completed, running free control...", tv_diff);
		err_log("-- -- -- -- -- -- -- --");
		err_log("-- --");
		++runs_kalman; // so re-entry doesn't happen
	    }
	}

	/// -- -- -- -- -- -- -- --
	/// Update setpoint
	/// -- -- -- -- -- -- -- --
	retval = pp_update_setpoint(pp, kalman->x_hat, mot->w_hover);
	log_n_continue(retval,"Kalman update failed");

	/// -- -- -- -- -- -- -- --
	/// Run control
	/// -- -- -- -- -- -- -- --
	gettimeofday(&tv_tmp,NULL);
	uquad_timeval_substract(&tv_diff,tv_tmp,tv_last_m_cmd);
	retval = control(ctrl, w, kalman->x_hat, pp->sp, (double)tv_diff.tv_usec);
	log_n_continue(retval,"Control failed!");
#if DEBUG
	uquad_timeval_substract(&tv_diff,tv_tmp,tv_start);
#endif // DEBUG
#if DEBUG && LOG_W_CTRL
	retval = uquad_mat_transpose(wt,w);
	log_tv_only(log_w_ctrl,tv_diff);
	uquad_mat_dump(wt,log_w_ctrl);
	fflush(log_w_ctrl);
#endif
#if LOG_INT
	log_tv_only(log_int, tv_diff);
	uquad_mat_dump_vec(ctrl->x_int, log_int, false);
#endif // LOG_INT

	/// -- -- -- -- -- -- -- --
	/// Set motor speed
	/// -- -- -- -- -- -- -- --
	gettimeofday(&tv_tmp,NULL);
	uquad_timeval_substract(&tv_diff,tv_tmp,tv_last_m_cmd);
	if (tv_diff.tv_usec > MOT_UPDATE_T || tv_diff.tv_sec > 1)
	{
	    /// Update motor controller
	    if(!(runs_kalman > STARTUP_KALMAN))
	    {
		/**
		 * Motors would start from hover speed
		 * Ramp them up, but keep controlling to maintain
		 * balance.
		 */
		for(i = 0; i < MOT_C; ++i)
		{
		    w_ramp->m_full[i] = w->m_full[i];
		    w->m_full[i] = uquad_max(mot->w_min,
					     w->m_full[i] - (STARTUP_KALMAN - runs_kalman)
					     *((mot->w_hover - mot->w_min)/STARTUP_KALMAN)
					     );
		}
	    }
	    retval = mot_set_vel_rads(mot, w, false);
	    log_n_continue(retval,"Failed to set motor speed!");
#if DEBUG && LOG_W
	    uquad_timeval_substract(&tv_diff,tv_tmp,tv_start);
	    log_tv_only(log_w,tv_diff);
	    retval = uquad_mat_transpose(wt,mot->w_curr);
	    log_n_continue(retval, "Failed to prepare w transpose...");
	    uquad_mat_dump(wt,log_w);
	    fflush(log_w);
#endif

#if LOG_BUKAKE
	    uquad_timeval_substract(&tv_diff,tv_tmp,tv_start);
	    log_tv_only(log_bukake,tv_diff);
	    int ind; uquad_mat_t *xm = kalman->x_hat; int len = xm->r*xm->c;
	    for(ind = 0; ind < len; ++ind)
		log_double_only(log_bukake,xm->m_full[ind]);
	    fdatasync(fileno(log_bukake));
	    retval = uquad_mat_transpose(wt,mot->w_curr);
	    uquad_mat_dump(wt,log_bukake);
	    fdatasync(fileno(log_bukake));
#endif
	    tv_last_m_cmd = tv_tmp;
	}

	/// -- -- -- -- -- -- -- --
	/// Check stdin
	/// -- -- -- -- -- -- -- --
	if(reg_stdin)
	{
	    retval = io_dev_ready(io,STDIN_FILENO,&read,NULL);
	    log_n_continue(retval, "Failed to check stdin for input!");
	    if(!read)
		continue;
	    input = !input;
	    log_n_continue(ERROR_FAIL, "STDIN NOT IMPLEMENTED!");
	    continue;
	    //	    input = getch();
	    if(input > 0 && !interrupted)
	    {
		gettimeofday(&tv_tmp,NULL);
		retval = uquad_timeval_substract(&tv_diff,tv_tmp,tv_start);
		if(retval <= 0)
		{
		    err_log("Absurd timing!");
		}
		retval = ERROR_OK; // clear error
		dtmp = 0.0;
		if(input == QUIT)
		{
		    err_log("Terminating program based on user input...");
		    quit(); // Kill motors
		    quit(); // Kill main.c
		    // never gets here
		}
		if(!manual_mode && (char)input != MANUAL_MODE)
		{
		    err_log("Manuel mode DISABLED, enable with 'm'. Ignoring input...");
		}
		else
		{
		    switch(input)
		    {
		    case MANUAL_MODE:
			// switch manual mode on/off
			if(pp == NULL)
			{
			    err_log("Cannot enable manual mode, path planner not setup!");
			}
			else
			{
			    manual_mode = !manual_mode;
			    if(manual_mode)
			    {
				err_log_tv("Manuel mode ENABLED!",tv_diff);
			    }
			    else
			    {
				err_log_tv("Manuel mode DISABLED!",tv_diff);
			    }
			}
			break;
		    case MANUAL_PSI_INC:
			if(pp == NULL)
			{
			    err_log("Path planner not setup!");
			}
			pp->sp->x->m_full[SV_PSI] += MANUAL_EULER_STEP;
			break;
		    case MANUAL_PSI_DEC:
			if(pp == NULL)
			{
			    err_log("Path planner not setup!");
			}
			pp->sp->x->m_full[SV_PSI] -= MANUAL_EULER_STEP;
			break;
		    case MANUAL_PHI_INC:
			if(pp == NULL)
			{
			    err_log("Path planner not setup!");
			}
			pp->sp->x->m_full[SV_PHI] += MANUAL_EULER_STEP;
			break;
		    case MANUAL_PHI_DEC:
			if(pp == NULL)
			{
			    err_log("Path planner not setup!");
			}
			pp->sp->x->m_full[SV_PHI] -= MANUAL_EULER_STEP;
			break;
		    case MANUAL_THETA_INC:
			if(pp == NULL)
			{
			    err_log("Path planner not setup!");
			}
			pp->sp->x->m_full[SV_THETA] += MANUAL_EULER_STEP;
			break;
		    case MANUAL_THETA_DEC:
			if(pp == NULL)
			{
			    err_log("Path planner not setup!");
			}
			pp->sp->x->m_full[SV_THETA] -= MANUAL_EULER_STEP;
			break;
		    case MANUAL_WEIGHT:
			retval = mot_update_w_hover(mot, MASA_DEFAULT);
			quit_log_if(retval, "Failed to update weight!");
			break;
		    case MANUAL_WEIGHT_INC:
			if(pp == NULL)
			{
			    err_log("Path planner not setup!");
			}
			dtmp = MANUAL_WEIGHT_STEP;
			break;
		    case MANUAL_WEIGHT_DEC:
			if(pp == NULL)
			{
			    err_log("Path planner not setup!");
			}
			dtmp = -MANUAL_WEIGHT_STEP;
			break;
		    case MANUAL_Z_INC:
			if(pp == NULL)
			{
			    err_log("Path planner not setup!");
			}
			pp->sp->x->m_full[SV_Z] += MANUAL_Z_STEP;
			break;
		    case MANUAL_Z_DEC:
			if(pp == NULL)
			{
			    err_log("Path planner not setup!");
			}
			pp->sp->x->m_full[SV_Z] -= MANUAL_Z_STEP;
			break;
		    default:
			err_log("Invalid input!");
			input = ERROR_INVALID_ARG;
			break;
		    }
		    if(dtmp != 0.0)
		    {
			retval = mot_update_w_hover(mot, mot->weight + dtmp);
			quit_log_if(retval, "Failed to update weight!");
			// display on screen
			log_tv_only(stdout,tv_diff);
			log_double(stdout,"Current w hover:",mot->w_hover);
			fflush(stdout);
		    }
		    else
		    {
			if(manual_mode && input != ERROR_INVALID_ARG)
			{
			    uquad_mat_dump_vec(pp->sp->x,stderr, true);
			}
		    }
#if LOG_TV
		    // save to log file
		    log_tv_only(log_tv, tv_diff);
		    log_double(log_tv,"Current w hover",mot->w_hover);
		    fflush(log_tv);
#endif
		}
	    }
	}
	retval = ERROR_OK;
    }
    // never gets here
    return 0;
}
    
