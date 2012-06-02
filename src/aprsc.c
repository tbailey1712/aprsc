/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

#define HELPS	"Usage: aprsc [-c cfgfile] [-f (fork)] [-n <logname>] [-e <loglevel>] [-o <logdest>] [-r <logdir>] [-h (help)]\n"

#include <pthread.h>
#include <semaphore.h>

#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#ifdef __linux__ /* Very Linux-specific code.. */
#include <sys/syscall.h>
#endif
#include <sys/resource.h>
#include <locale.h>

#include "hmalloc.h"
#include "hlog.h"
#include "config.h"
#include "accept.h"
#include "uplink.h"
#include "worker.h"
#include "status.h"

#include "dupecheck.h"
#include "filter.h"
#include "historydb.h"
#include "keyhash.h"

struct itimerval itv; // Linux profiling timer does not pass over to pthreads..

int shutting_down;		// are we shutting down now?
int reopen_logs;		// should we reopen log files now?
int reconfiguring;		// should we reconfigure now?
int uplink_simulator;
int fileno_limit;
int dbdump_at_exit;
int want_dbdump;

pthread_attr_t pthr_attrs;

/*
 *	Parse arguments
 */

void parse_cmdline(int argc, char *argv[])
{
	int s, i;
	int failed = 0;
	
	while ((s = getopt(argc, argv, "c:fn:r:d:De:o:?h")) != -1) {
	switch (s) {
		case 'c':
			cfgfile = hstrdup(optarg);
			break;
		case 'f':
			fork_a_daemon = 1;
			break;
		case 'n':
			logname = hstrdup(optarg);
			break;
		case 'r':
			log_dir = hstrdup(optarg);
			break;
		case 'd':
			if (!strcasecmp(optarg, "requests")) {
				//dump_requests = 1;
			} else if (!strcasecmp(optarg, "splay")) {
				dump_splay = 1;
			} else {
				fprintf(stderr, "Unknown -d parameter: %s\n", optarg);
				failed = 1;
			}
			break;
		case 'D':
			dbdump_at_exit = 1;
			break;
		case 'e':
			i = pick_loglevel(optarg, log_levelnames);
			if (i > -1)
				log_level = i;
			else {
				fprintf(stderr, "Log level unknown: \"%s\"\n", optarg);
				failed = 1;
			}
			break;
		case 'o':
			i = pick_loglevel(optarg, log_destnames);
			if (i > -1)
				log_dest = i;
			else {
				fprintf(stderr, "Log destination unknown: \"%s\"\n", optarg);
				failed = 1;
			}
			break;
		case '?':
		case 'h':
			fprintf(stderr, "%s", VERSTR);
			failed = 1;
	}
	}
	
	if ((log_dest == L_FILE) && (!log_dir)) {
		fprintf(stderr, "Log destination set to 'file' but no log directory specified!\n");
		failed = 1;
	}
	
	if (failed) {
		fputs(HELPS, stderr);
		exit(failed);
	}
}

/*
 *	signal handler
 */
 
int sighandler(int signum)
{
	switch (signum) {

	case SIGINT:
	case SIGTERM:
	case SIGQUIT:
		hlog(LOG_CRIT, "Shutting down on signal %d", signum);
		shutting_down = 1;
		return 0;
		
	case SIGHUP:
		hlog(LOG_INFO, "SIGHUP received: reopening logs");
		reopen_logs = 1;
		break;
		
	case SIGUSR1:
		hlog(LOG_INFO, "SIGUSR1 received: reconfiguring");
		reconfiguring = 1;
		break;
		
	case SIGUSR2:
		hlog(LOG_INFO, "SIGUSR2 received: database dumping");
		want_dbdump = 1;
		break;
		
	default:
		hlog(LOG_WARNING, "* SIG %d ignored", signum);
		break;
	}
	
	signal(signum, (void *)sighandler);	/* restore handler */
	return 0;
		
}

/*
 *	A very Linux specific thing, as there the pthreads are a special variation
 *	of fork(), and per POSIX the profiling timers are not kept over fork()...
 */
void pthreads_profiling_reset(const char *name)
{
#ifdef __linux__ /* Very Linux-specific code.. */
	int tid;
	if (itv.it_interval.tv_usec || itv.it_interval.tv_sec) {
	  setitimer(ITIMER_PROF, &itv, NULL);
	}

	tid = syscall(SYS_gettid);
	hlog(LOG_DEBUG, "Thread %s: Linux ThreadId: %d", name, tid);
#endif
}

#define PATHLEN 500
static void dbdump_all(void)
{
	FILE *fp;
	char path[PATHLEN+1];

	/*
	 *    As a general rule, dumping of databases is not a Good Idea in
	 *    operational system.  Development time debugging on other hand..
	 */

	snprintf(path, PATHLEN, "%s/historydb.dump", rundir);
	fp = fopen(path,"w");
	if (fp) {
		historydb_dump(fp);
		fclose(fp);
	}
	snprintf(path, PATHLEN, "%s/filter.wx.dump", rundir);
	fp = fopen(path,"w");
	if (fp) {
		filter_wx_dump(fp);
		fclose(fp);
	}
	snprintf(path, PATHLEN, "%s/filter.entry.dump", rundir);
	fp = fopen(path,"w");
	if (fp) {
		filter_entrycall_dump(fp);
		fclose(fp);
	}
	snprintf(path, PATHLEN, "%s/pbuf.dump", rundir);
	fp = fopen(path,"w");
	if (fp) {
		pbuf_dump(fp);
		fclose(fp);
	}
	snprintf(path, PATHLEN, "%s/pbuf.dupe.dump", rundir);
	fp = fopen(path,"w");
	if (fp) {
		pbuf_dupe_dump(fp);
		fclose(fp);
	}
}

/*
 *	Main
 */

int main(int argc, char **argv)
{
	pthread_t accept_th;
	int e;
	struct rlimit rlim;
	time_t cleanup_tick;
	time_t stats_tick;
	
	/* close stdin */
	close(0);
	time(&tick);
	now = tick;
	cleanup_tick = tick;
	stats_tick = tick;
	startup_tick = tick;
	setlinebuf(stdout);
	setlinebuf(stderr);
	
	/* set locale to C so that isalnum(), isupper() etc don't do anything
	 * unexpected if the environment is unexpected.
	 */
	if (!setlocale(LC_COLLATE, "C")) {
		hlog(LOG_CRIT, "Failed to set locale C for LC_COLLATE.");
		exit(1);
	}
	if (!setlocale(LC_CTYPE, "C")) {
		hlog(LOG_CRIT, "Failed to set locale C for LC_CTYPE.");
		exit(1);
	}
	if (!setlocale(LC_MESSAGES, "C")) {
		hlog(LOG_CRIT, "Failed to set locale C for LC_MESSAGES.");
		exit(1);
	}
	if (!setlocale(LC_NUMERIC, "C")) {
		hlog(LOG_CRIT, "Failed to set locale C for LC_NUMERIC.");
		exit(1);
	}

	/* Adjust process global fileno limit */
	e = getrlimit(RLIMIT_NOFILE, &rlim);
	rlim.rlim_cur = rlim.rlim_max;
	e = setrlimit(RLIMIT_NOFILE, &rlim);
	e = getrlimit(RLIMIT_NOFILE, &rlim);
	fileno_limit = rlim.rlim_cur;

	getitimer(ITIMER_PROF, &itv);
	
	/* command line */
	parse_cmdline(argc, argv);
	
	/* open syslog, write an initial log message and read configuration */
	open_log(logname, 0);
	hlog(LOG_NOTICE, "Starting up...");
	if (read_config()) {
		hlog(LOG_CRIT, "Initial configuration failed.");
		exit(1);
	}
	
	/* fork a daemon */
	if (fork_a_daemon) {
		int i = fork();
		if (i < 0) {
			hlog(LOG_CRIT, "Fork to background failed: %s", strerror(errno));
			fprintf(stderr, "Fork to background failed: %s\n", strerror(errno));
			exit(1);
		} else if (i == 0) {
			/* child */
		} else {
			/* parent, quitting */
			hlog(LOG_DEBUG, "Forked daemon process %d, parent quitting", i);
			exit(0);
		}
	}
	
	/* write pid file, now that we have our final pid... might fail, which is critical */
	if (!writepid(pidfile))
		exit(1);
	
	/* catch signals */
	signal(SIGINT, (void *)sighandler);
	signal(SIGTERM, (void *)sighandler);
	signal(SIGQUIT, (void *)sighandler);
	signal(SIGHUP, (void *)sighandler);
	signal(SIGUSR1, (void *)sighandler);
	signal(SIGUSR2, (void *)sighandler);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGURG, SIG_IGN);
	


	/* Early inits in single-thread mode */
	keyhash_init();
	filter_init();
	pbuf_init();
	dupecheck_init();
	historydb_init();
	client_init();
	xpoll_init();

	time(&cleanup_tick);

	pthread_attr_init(&pthr_attrs);
	/* 128 kB stack is enough for each thread,
	   default of 10 MB is way too much...*/
	pthread_attr_setstacksize(&pthr_attrs, 128*1024);

	/* start the accept thread, which will start server threads */
	if (pthread_create(&accept_th, &pthr_attrs, (void *)accept_thread, NULL))
		perror("pthread_create failed for accept_thread");

	/* act as statistics and housekeeping thread from now on */
	while (!shutting_down) {
		poll(NULL, 0, 300); // 0.300 sec -- or there abouts..
		time(&tick);
		if (!uplink_simulator)
			now = tick;

		if (want_dbdump) {
			dbdump_all();
			want_dbdump = 0;
		}
		
		if (reopen_logs) {
			reopen_logs = 0;
			close_log(1);
		}
		
		if (reconfiguring) {
			reconfiguring = 0;
			if (read_config()) {
				hlog(LOG_ERR, "New configuration was not successfully read.");
			} else {
				hlog(LOG_INFO, "New configuration read successfully. Applying....");
				accept_reconfiguring = 1;
			}
		}
		
		if (cleanup_tick < tick || cleanup_tick > tick + 60) {
			cleanup_tick = tick + 60;

			historydb_cleanup();
			filter_wx_cleanup();
			filter_entrycall_cleanup();
		}
		
		if (stats_tick < tick || stats_tick > tick + stats_interval) {
			stats_tick = tick + stats_interval;
			hlog(LOG_DEBUG, "Dumping status to file");
			status_dump_file();
		}
	}
	
	hlog(LOG_INFO, "Signalling accept_thread to shut down...");
	accept_shutting_down = 1;
	if ((e = pthread_join(accept_th, NULL)))
		hlog(LOG_ERR, "Could not pthread_join accept_th: %s", strerror(e));
	else
		hlog(LOG_INFO, "Accept thread has terminated.");
	
	if (dbdump_at_exit) {
		dbdump_all();
	}

	free_config();
	dupecheck_atend();
	historydb_atend();
	filter_wx_atend();
	filter_entrycall_atend();
	
	hlog(LOG_CRIT, "Shut down.");
	close_log(0);
	
	return 0;
}

