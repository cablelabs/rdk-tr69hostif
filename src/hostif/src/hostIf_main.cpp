/*
 * If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2016 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/



/**
* @defgroup tr69hostif
* @{
* @defgroup hostif
* @{
**/
#include "hostIf_main.h"
#include "hostIf_tr69ReqHandler.h"
//#include "hostIf_dsClient_ReqHandler.h"
#include "hostIf_jsonReqHandler.h"
#include "hostIf_updateHandler.h"

#if defined(USE_WIFI_PROFILE)
#include "Device_WiFi.h"
#endif

#if defined(PARODUS_ENABLE)
#include "libpd.h"
#endif

#include <semaphore.h>
#include <errno.h>
#include "libIBus.h"

#define LOG_FILE 		"./tr69hostIflog.txt"
#ifdef WEBPA_RFC_ENABLED
#define RFC_BUFFER_SIZE 256
#endif

#ifdef ENABLE_SD_NOTIFY
#include <systemd/sd-daemon.h>
#endif

//static void killAllThreads();

//------------------------------------------------------------------------------
// Initialize global variables and functions.
//------------------------------------------------------------------------------
GThread *hostIf_JsonIfThread = NULL;
//GTimeVal timeval;
static GMainLoop *main_loop = NULL;
gchar *date_str = NULL;
const gchar* logfilename = NULL;
FILE *logfile = NULL;
int rdk_logger_enabled = 0;
GHashTable* paramMgrhash = NULL;
static void usage();
T_ARGLIST argList = {{'\0'}, 0};

#if defined(PARODUS_ENABLE)
// Parodus Init Thread
pthread_t parodus_init_tid;
#endif


char *hostIf_JsonIfMsg = (char *)"hostIf_JsonIfThread";
#if defined(ENABLE_TELEMETRY_LOGGER)
pthread_t telemetery_tid;
char *telemetryLoggerThread = (char *)"telemetryLoggerThread";
#endif
GError *err1 = NULL ;

/* Globals associated with the shutdown thread which frees all resources and exits the process
   when signalled to do so */
static  pthread_t   shutdown_thread = 0;                /* The thread ID */
static  void        *shutdown_thread_entry(void *arg);  /* Thread entry function */
static  sem_t       shutdown_thread_sem;                /* Semaphore used to signal shutdown */
static  int         shutdown_sig_received = 0;          /* The signal that triggered the shutdown */
pthread_mutex_t     graceful_exit_mutex;
#define UNUSED(x)   ((void)(x))

#if defined(ENABLE_TELEMETRY_LOGGER)
/* Telemetry thread condition mutex */
pthread_cond_t cond_telemetry = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex_telemetry = PTHREAD_MUTEX_INITIALIZER;
#endif

//------------------------------------------------------------------------------
// shutdown_thread_entry: frees all resources and exits the process
//                        when signalled to do so
//------------------------------------------------------------------------------
static void *shutdown_thread_entry(void *arg)
{
    UNUSED(arg);

    /* Wait forever on the shutdown semaphore */
    while ((sem_wait(&shutdown_thread_sem) == -1) && (errno == EINTR))
    {
        continue;   /* Restart if interrupted by handler */
    }

     RDK_LOG(RDK_LOG_INFO,LOG_TR69HOSTIF,"[%s:%s] TR69 Host-If received signal handler with sigNum : %d \n",__FILE__, __FUNCTION__, shutdown_sig_received);

    switch (shutdown_sig_received)
    {
        case SIGINT:
             RDK_LOG(RDK_LOG_NOTICE,LOG_TR69HOSTIF,"SIGINT received (CTRL+C was pressed) \n");
            break;
        case SIGTERM:
             RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"SIGTERM received\n");
            break;
        case SIGQUIT:
            RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"SIGQUIT received\n");
            break;
        case SIGSEGV:
            RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"SIGSEGV received (Segmentation fault was detected)\n");
            break;
        default:
            RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"Signal %d received\n", shutdown_sig_received);
            break;
    }

    exit_gracefully(shutdown_sig_received);
}

#ifdef WEBPA_RFC_ENABLED
/* Utility funciton to get the console output. */
int GetFeatureEnabled(char *cmd)
{
    FILE * pipeStream = NULL;
    char buffer[RFC_BUFFER_SIZE];
    int isFeatureEnabled = 0;

    memset(buffer, 0, RFC_BUFFER_SIZE);
    pipeStream = popen(cmd, "r");
    if (pipeStream != NULL)
    {
        if (fgets(buffer, RFC_BUFFER_SIZE, pipeStream) != NULL)
            sscanf(buffer,"%d",&isFeatureEnabled);
        else
            RDK_LOG(RDK_LOG_ERROR, LOG_TR69HOSTIF,"[%s] %s End of stream.\n", __FUNCTION__, cmd);
        pclose(pipeStream);
    }
    return isFeatureEnabled;
}
#endif

//------------------------------------------------------------------------------
// main: HostIfMgr main
//------------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    int ch = 0;
#ifdef WEBPA_RFC_ENABLED
    int retVal=0;
#endif
    const char* debugConfigFile = NULL;
    const char* webpaNotifyConfigFile = NULL;
    //------------------------------------------------------------------------------
    // Signal handlers:
    //------------------------------------------------------------------------------
    struct sigaction sigact;
    sigset_t sigset;

    if (sem_init(&shutdown_thread_sem, 0, 0) == -1)
    {
        RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"[%s:%s] sem_init() failed\n", __FUNCTION__, __FILE__);
        return 1;
    }

    if (pthread_create(&shutdown_thread, NULL, shutdown_thread_entry, NULL) != 0)
    {
        RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"[%s:%s] pthread_create() failed\n", __FUNCTION__, __FILE__);
        return 1;
    }

    while (1)
    {
        static struct option long_options[] =
        {
            /* These options don't set a flag.
                We distinguish them by their indices. */
            {"help",    		no_argument, 0, 'h'},
            {"logfile",     	required_argument, 0, 'l'},
            {"conffile",     	required_argument, 0, 'c'},
            {"port",       		required_argument, 0, 'p'},
            {"debugconfig",     required_argument, 0, 'd'},
	    {"notifyconfig",    required_argument, 0, 'w'},
            {0, 0, 0, 0}
        };

        /* getopt_long stores the option index here. */
        int option_index = 0;

        ch = getopt_long (argc, argv, "hHl:c:p:d:w:",
                          long_options, &option_index);
        /* Detect the end of the options. */
        if (ch == -1)
            break;

        switch (ch)
        {
        case 'c':
            if(optarg)
            {
                memset(argList.confFile, '\0', sizeof (argList.confFile));
                strcpy (argList.confFile, optarg);
//                RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"argList.confFile : %s optarg : %s\n", argList.confFile, optarg);
            }
            break;

        case 'd':
            if(optarg)
            {
		debugConfigFile = optarg;
            }
            break;
        case 'w':
	    if(optarg)
	    {
		webpaNotifyConfigFile = optarg;
            }
	    break;

        case 'p':
            if(optarg)
            {
                argList.httpPort = atoi(optarg);
            }
            break;
        case 'h':
        case 'H':
        case '?':
        default:
            usage();
            exit(0);
        }
    }

    /* Enable RDK logger.*/
    if(rdk_logger_init(debugConfigFile) == 0) rdk_logger_enabled = 1;

    if (optind < argc)
    {
        RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"non-option ARGV-elements: ");
        while (optind < argc)
            RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"%s ", argv[optind++]);
        putchar ('\n');
        usage();
        exit (0);
    }

#ifdef WEBPA_RFC_ENABLED
    retVal = GetFeatureEnabled(". /lib/rdk/isFeatureEnabled.sh WEBPAXG");
    RDK_LOG(RDK_LOG_INFO, LOG_TR69HOSTIF,"[%s] WEBPAXG returns %d\n", __FUNCTION__, retVal);
    if( retVal == 0)
    {
        system("systemctl stop tr69hostif.service");
        return ch;
    }
#endif

    if (sem_init(&shutdown_thread_sem, 0, 0) == -1)
    {
        RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"[%s:%s] sem_init() failed\n", __FUNCTION__, __FILE__);
        return 1;
    }

    if (pthread_create(&shutdown_thread, NULL, shutdown_thread_entry, NULL) != 0)
    {
        RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"[%s:%s] pthread_create() failed\n", __FUNCTION__, __FILE__);
        return 1;
    }

    // The actions for SIGINT, SIGTERM, SIGSEGV, and SIGQUIT are set
    sigemptyset(&sigact.sa_mask);
    sigact.sa_handler = quit_handler;
    sigact.sa_flags = SA_ONSTACK;

    sigaction (SIGINT, &sigact, NULL);
    sigaction (SIGTERM, &sigact, NULL);
    sigaction (SIGHUP, &sigact, NULL);
#ifndef RDK_DEVICE_CISCO_XI4
#if 0
		sigaction (SIGSEGV, &sigact, NULL);
		sigaction (SIGILL, &sigact, NULL);
		sigaction (SIGFPE, &sigact, NULL);
		sigaction (SIGABRT, &sigact, NULL);
		sigaction (SIGQUIT, &sigact, NULL);
#endif
#endif
		signal (SIGPIPE, SIG_IGN);

		setvbuf(stdout, NULL, _IOLBF, 0);

    //------------------------------------------------------------------------------
    // Initialize the glib, g_time and logger
    //------------------------------------------------------------------------------
#if GLIB_VERSION_CUR_STABLE <= GLIB_VERSION_2_32
    if(!g_thread_supported())
    {
        g_thread_init(NULL);
        RDK_LOG(RDK_LOG_NOTICE,LOG_TR69HOSTIF,"g_thread supported\n");
    }
    else
    {
        RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"g_thread NOT supported\n");
    }
#endif
    /* Enable RDK logger.*/
    if(rdk_logger_init(debugConfigFile) == 0) rdk_logger_enabled = 1;

#if defined(USE_WIFI_PROFILE)
    /* Perform the necessary operations to initialise the WiFi device */
    (void)WiFiDevice::init();
#endif
//    g_get_current_time(&timeval);
//    char* logoutfile = (char *)LOG_FILE;
#if 0
    /* Commented: Since logs are directed to /opt/logs/ folder,
     * so no need to use separate log file */
    char* logoutfile = (char *)argList.logFileName;


    g_log_set_handler(G_LOG_DOMAIN, (GLogLevelFlags)(G_LOG_LEVEL_INFO | G_LOG_LEVEL_MESSAGE | \
                      G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL | \
                      G_LOG_LEVEL_ERROR), tr69hostIf_logger, (void *)logoutfile);
#endif
    RDK_LOG(RDK_LOG_NOTICE,LOG_TR69HOSTIF,"Starting tr69HostIf Service\n");



    /*Commented: This function will replace hostIf_initalize_ConfigManger()
     This shall read all configuration properties of hostif like, profile Manager List,
     any hardcoded path used in reboot/reset or config paths. This uses g_key_file_new() to read
     based on the group of configuration. */
    //if(false == hostIf_ConfigProperties_Init())

    if(false == hostIf_initalize_ConfigManger())
    {
        RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"Failed to hostIf_initalize_ConfigManger()\n");
    }

    if(false == hostIf_IARM_IF_Start() )
    {
        RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"Failed to start hostIf_IARM_IF_Start()\n");
    }

    //------------------------------------------------------------------------------
    // hostIf_HttpServerStart: Soup HTTP Server
    //------------------------------------------------------------------------------
    if( (hostIf_JsonIfThread = g_thread_create(    (GThreadFunc)jsonIfHandlerThread,
                               (void *)hostIf_JsonIfMsg,
                               FALSE,
                               &err1)) == NULL)
    {
        g_critical("Thread create failed: %s!!\n", err1->message );
        g_error_free (err1);
    }

    #ifdef ENABLE_SD_NOTIFY
        sd_notifyf(0, "READY=1\n"
           "STATUS=tr69hostif is Successfully Initialized\n"
              "MAINPID=%lu", (unsigned long) getpid());
        RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"tr69hostif sd notify envent  is sent  Successfully\n");
    #endif

#ifdef PID_FILE_PATH
#define xstr(s) str(s)
#define str(s) #s
    // write pidfile because sd_notify() does not work inside container
    IARM_Bus_WritePIDFile(xstr(PID_FILE_PATH) "/tr69hostif.pid");
#endif

    //------------------------------------------------------------------------------
    // updateHandler::init :  Update handler thread for polling table profiles
    //------------------------------------------------------------------------------
    updateHandler::Init();

	//------------------------------------------------------------------------------
	// Telemetry logger start
	//------------------------------------------------------------------------------
#if defined(ENABLE_TELEMETRY_LOGGER)
    if(0 == pthread_create(&telemetery_tid, NULL, telemetryLogger_thFunc, NULL))
    {
        RDK_LOG(RDK_LOG_INFO,LOG_TR69HOSTIF,"TelemetryThread created Successfully\n");
    }
    else
    {
        RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"TelemetryThread create failed\n");
    }
#endif

    //------------------------------------------------------------------------------
    // Initialize WebPA Module 
    //------------------------------------------------------------------------------
#if defined(PARODUS_ENABLE)

RDK_LOG(RDK_LOG_DEBUG,LOG_TR69HOSTIF,"Starting WEBPA Parodus Connections\n");
libpd_set_notifyConfigFile(webpaNotifyConfigFile);
if(0 == pthread_create(&parodus_init_tid, NULL, libpd_client_mgr, NULL))
{
	RDK_LOG(RDK_LOG_INFO,LOG_TR69HOSTIF,"Initiating Connection with PARODUS success.. \n");
}
else
{
	RDK_LOG(RDK_LOG_ERROR,LOG_TR69HOSTIF,"Parodus init thread create failed\n");
}
#endif
    main_loop = g_main_loop_new (NULL, FALSE);

    g_main_loop_run(main_loop);
    g_main_loop_unref (main_loop);

    return ch;
}

//------------------------------------------------------------------------------
// returns id of current thread, which called this function
//------------------------------------------------------------------------------
pid_t getTid()
{
    return (pid_t)syscall(__NR_gettid);
}

//------------------------------------------------------------------------------
// quit_handler: signal the shutdown thread to log and exit
//
// NOTE: This function is called in signal handler context, meaning that we can
//       only call a small set of functions that are deemed async-signal-safe.
//       Refer to the "overview of signals" man page (man 7 signal) to view the
//       set of functions. It's probably worth pointing out that printf() is NOT
//       part of the set of async-signal-safe functions.
//------------------------------------------------------------------------------
void quit_handler (int sig_received)
{
    /* Save the received signal and increment the shutdown semaphore to instruct
       the shutdown thread to log and exit */
    shutdown_sig_received = sig_received;
    (void)sem_post(&shutdown_thread_sem);
}

//------------------------------------------------------------------------------
// exit_gracefully: close all global file descriptors, ports, clear and exit
//------------------------------------------------------------------------------

void exit_gracefully (int sig_received)
{

    if(pthread_mutex_trylock(&graceful_exit_mutex) == 0) {
        RDK_LOG(RDK_LOG_NOTICE,LOG_TR69HOSTIF,"[%s:%s] Entering..\n", __FUNCTION__, __FILE__);

#if defined(USE_WIFI_PROFILE)
        /* Perform the necessary operations to shut down the WiFi device */
        WiFiDevice::shutdown();
#endif

#if defined(ENABLE_TELEMETRY_LOGGER)
        pthread_kill(telemetery_tid, sig_received);
#endif

#if defined(PARODUS_ENABLE)
        // Kill Parodus Thread
        pthread_kill(parodus_init_tid, sig_received);
#endif
       /*Stop libSoup server and exit Json Thread */
       hostIf_HttpServerStop();


       updateHandler::stop();

       if(logfile) fclose (logfile);

       if(paramMgrhash) {
            g_hash_table_destroy(paramMgrhash);
            paramMgrhash = NULL;
       }
       hostIf_IARM_IF_Stop();

       RDK_LOG(RDK_LOG_NOTICE,LOG_TR69HOSTIF,"[%s:%s] Exiting program gracefully..\n", __FUNCTION__, __FILE__);
       pthread_mutex_unlock(&graceful_exit_mutex);
       exit (0);
    }
}

//------------------------------------------------------------------------------
// hostIf_logger: logged the messages
//------------------------------------------------------------------------------
#if 0
void tr69hostIf_logger (const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data)
{
    logfilename = (gchar *)user_data;

    if(NULL == logfile) {
        logfile = g_fopen (logfilename, "a");
        // Fall back to console output if unable to open file
        g_print ("%s : %s\n", date_str,(char *)message);
        return;
    }
    date_str = g_time_val_to_iso8601(&timeval);
    g_fprintf (logfile, "%s : %s\n", date_str, message);

    if(date_str) {
        g_free (date_str);
    }

    RDK_LOG(RDK_LOG_TRACE1,LOG_TR69HOSTIF,"[%s:%s] Exiting..\n", __FUNCTION__, __FILE__);
}
#endif
//------------------------------------------------------------------------------
// hostIf_logger: logged the messages
//------------------------------------------------------------------------------
static void usage()
{
    cout << "\nUsage: tr69hostIf -p[http port] [-l][LogFileName]\n\
        \nDisplay tr69hostIf options:\n\
        -p  HTTP PORT			HTTP Server port number.\n\
        -l  LOG FILE NAME   		Output log file name with path.\n\
    	-c  CONFIG FILE NAME   		Configure file contains Manager list .\n\
	-d  DEBUG CONFIG FILE NAME 	Config file contain debug configurations. \n\
        -h             			Help\n\
        \nExamples:\n\
        ================================================================================\n\
    	tr69hostIf -l /opt/logs/tr69hostIf.log -c mgrlist.conf -p 5001 -d /etc/debug.ini\n\
        ================================================================================\n\
        \n" << endl;
}


/** @} */
/** @} */
