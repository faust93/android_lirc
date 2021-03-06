
/****************************************************************************
 ** irrecord.c **************************************************************
 ****************************************************************************
 *
 * irrecord -  application for recording IR-codes for usage with lircd
 *
 * Copyright (C) 1998,99 Christoph Bartelmus <lirc@bartelmus.de>
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <syslog.h>

#include "lirc_private.h"

void flushhw(void);
int resethw(void);
static int mywaitfordata(__u32 maxusec);
int availabledata(void);
int get_toggle_bit_mask(struct ir_remote *remote);
void set_toggle_bit_mask(struct ir_remote *remote, ir_code xor);
void get_pre_data(struct ir_remote *remote);
void get_post_data(struct ir_remote *remote);
typedef void (*remote_func) (struct ir_remote * remotes);

void for_each_remote(struct ir_remote *remotes, remote_func func);
void analyse_remote(struct ir_remote *raw_data);
void remove_pre_data(struct ir_remote *remote);
void remove_post_data(struct ir_remote *remote);
void invert_data(struct ir_remote *remote);
void remove_trail(struct ir_remote *remote);
int get_lengths(struct ir_remote *remote, int force, int interactive);
struct lengths *new_length(lirc_t length);
int add_length(struct lengths **first, lirc_t length);
void free_lengths(struct lengths **firstp);
void merge_lengths(struct lengths *first);
void get_scheme(struct ir_remote *remote, int interactive);
struct lengths *get_max_length(struct lengths *first, unsigned int *sump);
void unlink_length(struct lengths **first, struct lengths *remove);
int get_trail_length(struct ir_remote *remote, int interactive);
int get_lead_length(struct ir_remote *remote, int interactive);
int get_repeat_length(struct ir_remote *remote, int interactive);
int get_header_length(struct ir_remote *remote, int interactive);
int get_data_length(struct ir_remote *remote, int interactive);
int get_gap_length(struct ir_remote *remote);
void fprint_copyright(FILE * fout);

extern struct ir_remote *last_remote;

struct ir_remote remote;
struct ir_ncode ncode;

#define IRRECORD_VERSION "$Revision: 5.101 $"
#define BUTTON 80+1
#define RETRIES 10

#define min(a,b) (a>b ? b:a)
#define max(a,b) (a>b ? a:b)

/* the longest signal I've seen up to now was 48-bit signal with header */

#define MAX_SIGNALS 200

lirc_t signals[MAX_SIGNALS];

unsigned int eps = 30;
lirc_t aeps = 100;

/* some threshold values */

#define TH_SPACE_ENC   80	/* I want less than 20% mismatches */
#define TH_HEADER      90
#define TH_REPEAT      90
#define TH_TRAIL       90
#define TH_LEAD        90
#define TH_IS_BIT      10
#define TH_RC6_SIGNAL 550

#define MIN_GAP  20000
#define MAX_GAP 100000

#define SAMPLES 80

// Actual loglevel as per -D option, see lirc_log.h.
loglevel_t loglevel = LIRC_WARNING;

int daemonized = 0;

struct ir_remote *emulation_data;
struct ir_ncode *next_code = NULL;
struct ir_ncode *current_code = NULL;
int current_index = 0;
int current_rep = 0;


static struct option long_options[] = {
	{"help", no_argument, NULL, 'h'},
	{"version", no_argument, NULL, 'v'},
	{"analyse", no_argument, NULL, 'a'},
	{"device", required_argument, NULL, 'd'},
	{"options-file", required_argument, NULL, 'O'},
	{"debug", required_argument, NULL, 'D'},
	{"loglevel", required_argument, NULL, 'D'},
	{"driver", required_argument, NULL, 'H'},
	{"force", no_argument, NULL, 'f'},
	{"disable-namespace", no_argument, NULL, 'n'},
	{"list-namespace", no_argument, NULL, 'l'},
	{"plugindir", required_argument, NULL, 'U'},
	{"dynamic-codes", no_argument, NULL, 'Y'},
	{"pre", no_argument, NULL, 'p'},
	{"post", no_argument, NULL, 'P'},
	{"test", no_argument, NULL, 't'},
	{"invert", no_argument, NULL, 'i'},
	{"trail", no_argument, NULL, 'T'},
	{0, 0, 0, 0}
};

#define USAGE	    "Usage: irrecord [options] [config file]\n" \
		    "	    irrecord -a <config file> \n" \
		    "	    irrecord -l \n"

static const char* const help =
USAGE
"\nOptions:\n"
"\t -H --driver=driver\tUse given driver\n"
"\t -d --device=device\tRead from given device\n"
"\t -a --analyse\t\tAnalyse raw_codes config files\n"
"\t -l --list-namespace\tList valid button names\n"
"\t -U --plugindir=dir\tLoad drivers from dir\n"
"\t -f --force\t\tForce raw mode\n"
"\t -n --disable-namespace\tDisable namespace checks\n"
"\t -Y --dynamic-codes\tEnable dynamic codes\n"
"\t -D --loglevel=level\t'error', 'info', 'notice',... or 3..10\n"
"\t -h --help\t\tDisplay this message\n"
"\t -v --version\t\tDisplay version\n";


static void get_commandline(int argc, char** argv, char* buff, size_t size)
{
	int i;
	int j;
	int dest = 0;
	if (size == 0)
		return;
	for (i = 1; i < argc; i += 1 ) {
		for (j=0; argv[i][j] != '\0'; j += 1) {
			if (dest  + 1 >= size)
				break;
			buff[dest++] = argv[i][j];
		}
		if (dest  + 1 >= size)
			break;
		buff[dest++] = ' ';
	}
	buff[--dest] = '\0';
}

lirc_t emulation_readdata(lirc_t timeout)
{
	static lirc_t sum = 0;
	lirc_t data = 0;

	if (current_code == NULL) {
		data = 1000000;
		if (next_code) {
			current_code = next_code;
		} else {
			current_code = emulation_data->codes;
		}
		current_rep = 0;
		sum = 0;
	} else {
		if (current_code->name == NULL) {
			fprintf(stderr, "%s: %s no data found\n", progname, emulation_data->name);
			data = 0;
		}
		if (current_index >= current_code->length) {
			if (next_code) {
				current_code = next_code;
			} else {
				current_rep++;
				if (current_rep > 2) {
					current_code++;
					current_rep = 0;
					data = 1000000;
				}
			}
			current_index = 0;
			if (current_code->name == NULL) {
				current_code = NULL;
				return emulation_readdata(timeout);
			}
			if (data == 0) {
				if (is_const(emulation_data)) {
					data = emulation_data->gap - sum;
				} else {
					data = emulation_data->gap;
				}
			}

			sum = 0;
		} else {
			data = current_code->signals[current_index];
			if ((current_index % 2) == 0) {
				data |= PULSE_BIT;
			}
			current_index++;
			sum += data & PULSE_MASK;
		}
	}
	/*
	   printf("delivering: %c%u\n", data&PULSE_BIT ? 'p':'s',
	   data&PULSE_MASK);
	 */
	return data;
}

struct driver hw_emulation = {
	.name		= "emulation",
	.device		= "/dev/null",
	.features	= LIRC_CAN_REC_MODE2,
	.send_mode	= 0,
	.rec_mode	= LIRC_MODE_MODE2,
	.code_length	= 0,
	.init_func	= NULL,
	.deinit_func	= NULL,
	.send_func	= NULL,
	.rec_func	= NULL,
	.decode_func	= NULL,
	.drvctl_func	= NULL,
	.readdata	= emulation_readdata,
	.open_func	= default_open,
	.close_func	= default_close,
	.api_version	= 2,
	.driver_version = "0.9.2"
};

static int i_printf(int interactive, char *format_str, ...)
{
	va_list ap;
	int ret = 0;

	if (interactive && lirc_log_is_enabled_for(LIRC_DEBUG))
	{
		va_start(ap, format_str);
		ret = vfprintf(stdout, format_str, ap);
		va_end(ap);
	}
	return ret;
}

void dosigterm(int sig)
{
	raise(SIGTERM);
}

static void add_defaults(void)
{
	char level[4];
	snprintf(level, sizeof(level), "%d", lirc_log_defaultlevel());
	const char* const defaults[] = {
		"lircd:plugindir",		PLUGINDIR,
		"irrecord:driver",		"devinput",
		"irrecord:device",		 LIRC_DRIVER_DEVICE,
		"irrecord:analyse",		"False",
		"irrecord:force",		"False",
		"irrecord:disable-namespace",	"False",
		"irrecord:dynamic-codes",	"False",
		"irrecord:list_namespace",	"False",
		"lircd:debug",			 level,
		(const char*)NULL,	(const char*)NULL
	};
	options_add_defaults(defaults);
}

static void parse_options(int argc, char** const argv)
{
	int c;
	char* level;

	const char* const optstring = "had:D:H:fnlO:pPtiTU:vY";

	add_defaults();
	optind = 1;
	while ((c = getopt_long(argc, argv, optstring, long_options, NULL))
		!= -1 )
	{
		switch (c) {
		case 'a':
			options_set_opt("irrecord:analyse", "True");
			break;
		case 'D':
			level = optarg ? optarg : "debug";
			if (options_set_loglevel(level) == LIRC_BADLEVEL){
				fprintf(stderr,
					"Bad debug level: %s\n", optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 'd':
			options_set_opt("irrecord:device", optarg);
			break;
		case 'f':
			options_set_opt("irrecord:force", "True");
			break;
		case 'H':
			options_set_opt("irrecord:driver", optarg);
			break;
		case 'h':
			printf(help);
			exit(EXIT_SUCCESS);
		case 'i':
			options_set_opt("irrecord:invert", "True");
			break;
		case 'l':
			options_set_opt("irrecord:list-namespace", "True");
			break;
		case 'n':
			options_set_opt("irrecord:disable-namespace", "True");
			break;
		case 'O':
			return;
		case 'P':
			options_set_opt("irrecord:post", "True");
			break;
		case 'p':
			options_set_opt("irrecord:pre", "True");
			break;
		case 't':
			options_set_opt("irrecord:test", "True");
			break;
		case 'T':
			options_set_opt("irrecord:trail", "True");
			break;
		case 'U':
			options_set_opt("lircd:plugindir", optarg);
			break;
		case 'Y':
			options_set_opt("lircd:dynamic-codes", "True");
			break;
		case 'v':
			printf("irrecord %s\n",  VERSION);
			exit(EXIT_SUCCESS);
		default:
			fprintf(stderr, USAGE);
			exit(EXIT_FAILURE);
		}
	}
	if (optind == argc - 1) {
		options_set_opt("configfile", argv[optind]);
	} else if (optind != argc) {
		fprintf(stderr, "irrecord: invalid argument count\n");
		exit(EXIT_FAILURE);
	}
}



int main(int argc, char **argv)
{
	char *filename;
	FILE *fout, *fin;
	int flags;
	int retval = EXIT_SUCCESS;
	struct decode_ctx_t decode_ctx;
	int force;
	int disable_namespace = 0;
	int retries;
	int no_data = 0;
	struct ir_remote *remotes = NULL;
	const char *device = NULL;
	int using_template = 0;
	int analyse = 0;
	const char* opt;
	char commandline[128];
	char path[128];
	int get_pre = 0, get_post = 0, test = 0, invert = 0, trail = 0;

	get_commandline(argc, argv, commandline, sizeof(commandline));
	force = 0;
	hw_choose_driver(NULL);
	options_load(argc, argv, NULL, parse_options);
	opt = options_getstring("irrecord:driver");
	analyse = options_getboolean("irrecord:analyse");
	if (hw_choose_driver(opt) != 0 && ! analyse) {
		fprintf(stderr, "Driver `%s' not found", opt);
		fprintf(stderr, " (wrong or missing -U/--plugindir?).\n");
		hw_print_drivers(stderr);
		exit(EXIT_FAILURE);
	}
	device = options_getstring("irrecord:device");
	opt = options_getstring("lircd:debug");
	loglevel = string2loglevel(opt);
	if (loglevel < LIRC_MIN_LOGLEVEL || loglevel > LIRC_MAX_LOGLEVEL){
		fprintf(stderr, "Bad debug value %s\n", opt);
		exit(EXIT_FAILURE);
	}
	force = options_getboolean("irrecord:force");
	disable_namespace = options_getboolean("irrecord:disable-namespace");
	if (options_getboolean("irrecord:list-namespace")){
		fprint_namespace(stdout);
		exit(EXIT_SUCCESS);
	}
	ir_remote_init(options_getboolean("lircd:dynamic-codes"));
	get_pre = options_getboolean("irrecord:pre");
	get_post = options_getboolean("irrecord:post");
	test = options_getboolean("irrecord:test");
	invert = options_getboolean("irrecord:invert");
	trail = options_getboolean("irrecord:trail");
	lirc_log_get_clientlog("irrecord", path, sizeof(path));
	lirc_log_set_file(path);
	lirc_log_open("irrecord", 0, loglevel);
	curr_driver->open_func(device);
	if (strcmp(curr_driver->name, "null") == 0 && !analyse) {
		fprintf(stderr,
		       "%s: irrecord does not make sense without hardware\n",
			progname);
		exit(EXIT_FAILURE);
	}
	filename = argv[optind];
	if (filename == (char*) NULL)
		filename = "irrecord.conf";
	fin = fopen(filename, "r");
	if (fin != NULL) {
		char *filename_new;

		if (force) {
			fprintf(stderr,
				"%s: file \"%s\" already exists\n" "%s: you cannot use the --force option "
				"together with a template file\n", progname, filename, progname);
			exit(EXIT_FAILURE);
		}
		remotes = read_config(fin, filename);
		fclose(fin);
		if (remotes == (void *)-1 || remotes == NULL) {
			fprintf(stderr, "%s: file \"%s\" does not contain valid data\n", progname, filename);
			exit(EXIT_FAILURE);
		}
		if (analyse) {
			memcpy((void*)curr_driver, &hw_emulation, sizeof(struct driver));
			// hw = hw_emulation;
			for_each_remote(remotes, analyse_remote);
			return EXIT_SUCCESS;
		}
		using_template = 1;
		if (test) {
			if (trail)
				for_each_remote(remotes, remove_trail);
			for_each_remote(remotes, remove_pre_data);
			for_each_remote(remotes, remove_post_data);
			if (get_pre)
				for_each_remote(remotes, get_pre_data);
			if (get_post)
				for_each_remote(remotes, get_post_data);
			if (invert)
				for_each_remote(remotes, invert_data);

			fprint_remotes(stdout, remotes, commandline);
			free_config(remotes);
			return (EXIT_SUCCESS);
		}
		remote = *remotes;
		remote.name = NULL;
		remote.codes = NULL;
		remote.last_code = NULL;
		remote.next = NULL;
		if (remote.pre_p == 0 && remote.pre_s == 0 && remote.post_p == 0 && remote.post_s == 0) {
			remote.bits = bit_count(&remote);
			remote.pre_data_bits = 0;
			remote.post_data_bits = 0;
		}
		if (remotes->next != NULL) {
			fprintf(stderr, "%s: only first remote definition in file \"%s\" used\n", progname,
				filename);
		}
		filename_new = malloc(strlen(filename) + 10);
		if (filename_new == NULL) {
			fprintf(stderr, "%s: out of memory\n", progname);
			exit(EXIT_FAILURE);
		}
		strcpy(filename_new, filename);
		strcat(filename_new, ".conf");
		filename = filename_new;
	} else {
		if (analyse) {
			fprintf(stderr, "%s: no input file given, ignoring analyse flag\n", progname);
			analyse = 0;
		}
	}
	fout = fopen(filename, "w");
	if (fout == NULL) {
		fprintf(stderr, "%s: could not open new config file %s\n",
			progname, filename);
		perror(progname);
		exit(EXIT_FAILURE);
	}
	printf("\nirrecord -  application for recording IR-codes" " for usage with lirc\n" "\n"
	       "Copyright (C) 1998,1999 Christoph Bartelmus" "(lirc@bartelmus.de)\n");
	printf("\n");

	if (curr_driver->init_func) {
		if (!curr_driver->init_func()) {
			fprintf(stderr,
				"%s: could not init hardware" " (lircd running ? --> close it, check permissions)\n",
				progname);
			fclose(fout);
			unlink(filename);
			exit(EXIT_FAILURE);
		}
	}
	aeps = (curr_driver->resolution > aeps ? curr_driver->resolution : aeps);

	if (curr_driver->rec_mode != LIRC_MODE_MODE2 && curr_driver->rec_mode != LIRC_MODE_LIRCCODE) {
		fprintf(stderr, "%s: mode not supported\n", progname);
		fclose(fout);
		unlink(filename);
		if (curr_driver->deinit_func)
			curr_driver->deinit_func();
		exit(EXIT_FAILURE);
	}

	flags = fcntl(curr_driver->fd, F_GETFL, 0);
	if (flags == -1 || fcntl(curr_driver->fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		fprintf(stderr, "%s: could not set O_NONBLOCK flag\n", progname);
		fclose(fout);
		unlink(filename);
		if (curr_driver->deinit_func)
			curr_driver->deinit_func();
		exit(EXIT_FAILURE);
	}

	printf("This program will record the signals from your remote control\n"
	       "and create a config file for lircd.\n\n" "\n");
	if (curr_driver->name && strcmp(curr_driver->name, "devinput") == 0) {
		printf("Usually it's not necessary to create a new config file for devinput\n"
		       "devices. A generic config file can be found at:\n"
		       "https://sourceforge.net/p/lirc-remotes/code/ci/master/tree/remotes/devinput/devinput.lircd.conf"
		       "It can be downloaded using irdb-get(1)\n"
		       "Please try this config file before creating your own.\n" "\n");
	}
	printf("A proper config file for lircd is maybe the most vital part of this\n"
	       "package, so you should invest some time to create a working config\n"
	       "file. Although I put a good deal of effort in this program it is often\n"
	       "not possible to automatically recognize all features of a remote\n"
	       "control. Often short-comings of the receiver hardware make it nearly\n"
	       "impossible. If you have problems to create a config file check the wiki\n"
	       "at https://sourceforge.net/p/lirc-remotes/wiki, the Checklist and the\n"
	       "manual page."
	       "\n"
	       "If there already is a remote control of the same brand available at\n"
	       "http://www.lirc.org/remotes/ you might also want to try using such a\n"
	       "remote as a template. The config files already contain all\n"
	       "parameters of the protocol used by remotes of a certain brand and\n"
	       "knowing these parameters makes the job of this program much\n"
	       "easier. There are also template files for the most common protocols\n"
	       "available in the remotes/generic/ directory of the source\n"
	       "distribution of this package. You can use a template files by\n"
	       "providing the path of the file as command line parameter.\n"
	       "\n"
	       "Please take the time to finish this file as described in\n"
	       "https://sourceforge.net/p/lirc-remotes/wiki/Checklist/\n"
	       "and make it available to others by sending it to \n"
	       "<lirc@bartelmus.de>\n\n"
	       "Press RETURN to continue.\n");

	getchar();

	remote.name = filename;
	switch (curr_driver->rec_mode) {
	case LIRC_MODE_MODE2:
		remote.driver = NULL;
		if (!using_template && !get_lengths(&remote, force, 1)) {
			if (remote.gap == 0) {
				fprintf(stderr, "%s: gap not found," " can't continue\n", progname);
				fclose(fout);
				unlink(filename);
				if (curr_driver->deinit_func)
					curr_driver->deinit_func();
				exit(EXIT_FAILURE);
			}
			printf("Creating config file in raw mode.\n");
			set_protocol(&remote, RAW_CODES);
			remote.eps = eps;
			remote.aeps = aeps;
			break;
		}
		if lirc_log_is_enabled_for(LIRC_DEBUG) {
			 printf("%d %u %u %u %u %u %d %d %d %u\n",
				remote.bits, (__u32) remote.pone, (__u32) remote.sone, (__u32) remote.pzero,
				(__u32) remote.szero, (__u32) remote.ptrail, remote.flags, remote.eps,
				remote.aeps, (__u32) remote.gap);
		}
		break;
	case LIRC_MODE_LIRCCODE:
		remote.driver = curr_driver->name;
		remote.bits = curr_driver->code_length;
		if (!using_template && !get_gap_length(&remote)) {
			fprintf(stderr, "%s: gap not found," " can't continue\n", progname);
			fclose(fout);
			unlink(filename);
			if (curr_driver->deinit_func)
				curr_driver->deinit_func();
			exit(EXIT_FAILURE);
		}
		break;
	}

	if (!using_template && is_rc6(&remote)) {
		sleep(1);
		while (availabledata()) {
			curr_driver->rec_func(NULL);
		}
		if (!get_toggle_bit_mask(&remote)) {
			printf("But I know for sure that RC6 has a toggle bit!\n");
			fclose(fout);
			unlink(filename);
			if (curr_driver->deinit_func)
				curr_driver->deinit_func();
			exit(EXIT_FAILURE);
		}
	}
	printf("Now enter the names for the buttons.\n");

	fprint_copyright(fout);
	fprint_comment(fout, &remote, commandline);
	fprint_remote_head(fout, &remote);
	fprint_remote_signal_head(fout, &remote);
	while (1) {
		char buffer[BUTTON];
		char *string;

		if (no_data) {
			fprintf(stderr, "%s: no data for 10 secs," " aborting\n", progname);
			printf("The last button did not seem to generate any signal.\n");
			printf("Press RETURN to continue.\n\n");
			getchar();
			no_data = 0;
		}
		printf("\nPlease enter the name for the next button (press <ENTER> to finish recording)\n");
		string = fgets(buffer, BUTTON, stdin);

		if (string != buffer) {
			fprintf(stderr, "%s: fgets() failed\n", progname);
			retval = EXIT_FAILURE;
			break;
		}
		buffer[strlen(buffer) - 1] = 0;
		if (strchr(buffer, ' ') || strchr(buffer, '\t')) {
			printf("The name must not contain any whitespace.\n");
			printf("Please try again.\n");
			continue;
		}
		if (strcasecmp(buffer, "begin") == 0 || strcasecmp(buffer, "end") == 0) {
			printf("'%s' is not allowed as button name\n", buffer);
			printf("Please try again.\n");
			continue;
		}
		if (strlen(buffer) == 0) {
			break;
		}
		if (!disable_namespace && !is_in_namespace(buffer)) {
			printf("'%s' is not in name space (use --disable-namespace to disable checks)\n", buffer);
			printf("Use '%s --list-namespace' to see a full list of valid button names\n", progname);
			printf("Please try again.\n");
			continue;
		}

		if (is_raw(&remote)) {
			flushhw();
		} else {
			while (availabledata()) {
				curr_driver->rec_func(NULL);
			}
		}
		printf("\nNow hold down button \"%s\".\n", buffer);
		fflush(stdout);

		if (is_raw(&remote)) {
			lirc_t data, sum;
			unsigned int count;

			count = 0;
			sum = 0;
			while (count < MAX_SIGNALS) {
				__u32 timeout;

				if (count == 0)
					timeout = 10000000;
				else
					timeout = remote.gap * 5;
				data = curr_driver->readdata(timeout);
				if (!data) {
					if (count == 0) {
						no_data = 1;
						break;
					}
					data = remote.gap;
				}
				if (count == 0) {
					if (!is_space(data) || data < remote.gap - remote.gap * remote.eps / 100) {
						printf("Sorry, something went wrong.\n");
						sleep(3);
						printf("Try again.\n");
						flushhw();
						count = 0;
						continue;
					}
				} else {
					if (is_space(data)
					    && (is_const(&remote) ? data >
						(remote.gap > sum ? (remote.gap - sum) * (100 - remote.eps) / 100 : 0)
						: data > remote.gap * (100 - remote.eps) / 100)) {
						printf("Got it.\n");
						printf("Signal length is %d\n", count - 1);
						if (count % 2) {
							printf("That's weird because the signal length "
							       "must be odd!\n");
							sleep(3);
							printf("Try again.\n");
							flushhw();
							count = 0;
							continue;
						} else {
							ncode.name = buffer;
							ncode.length = count - 1;
							ncode.signals = signals;
							fprint_remote_signal(fout, &remote, &ncode);
							break;
						}
					}
					signals[count - 1] = data & PULSE_MASK;
					sum += data & PULSE_MASK;
				}
				count++;
			}
			if (count == MAX_SIGNALS) {
				printf("Signal is too long.\n");
			}
			if (retval == EXIT_FAILURE)
				break;
			continue;
		}
		retries = RETRIES;
		while (retries > 0) {
			int flag;

			if (!mywaitfordata(10000000)) {
				no_data = 1;
				break;
			}
			last_remote = NULL;
			flag = 0;
			sleep(1);
			while (availabledata()) {
				curr_driver->rec_func(NULL);
				if (curr_driver->decode_func(&remote, &decode_ctx)) {
					flag = 1;
					break;
				}
			}
			if (flag) {
				ir_code code2;

				ncode.name = buffer;
				ncode.code = decode_ctx.code;
				curr_driver->rec_func(NULL);
				if (curr_driver->decode_func(&remote, &decode_ctx)) {
					code2 = decode_ctx.code;
					decode_ctx.code = ncode.code;
					if (decode_ctx.code != code2) {
						ncode.next = malloc(sizeof(*(ncode.next)));
						if (ncode.next) {
							memset(ncode.next, 0, sizeof(*(ncode.next)));
							ncode.next->code = code2;
						}
					}
				}
				fprint_remote_signal(fout, &remote, &ncode);
				if (ncode.next) {
					free(ncode.next);
					ncode.next = NULL;
				}
				break;
			} else {
				printf("Something went wrong. ");
				if (retries > 1) {
					fflush(stdout);
					sleep(3);
					if (!resethw()) {
						fprintf(stderr, "%s: Could not reset hardware.\n", progname);
						retval = EXIT_FAILURE;
						break;
					}
					flushhw();
					printf("Please try again. (%d retries left)\n", retries - 1);
				} else {
					printf("\n");
					printf("Try using the -f option.\n");
				}
				retries--;
				continue;
			}
		}
		if (retries == 0)
			retval = EXIT_FAILURE;
		if (retval == EXIT_FAILURE)
			break;
	}
	fprint_remote_signal_foot(fout, &remote);
	fprint_remote_foot(fout, &remote);
	fclose(fout);

	if (retval == EXIT_FAILURE) {
		if (curr_driver->deinit_func)
			curr_driver->deinit_func();
		exit(EXIT_FAILURE);
	}

	if (is_raw(&remote)) {
		return (EXIT_SUCCESS);
	}
	if (!resethw()) {
		fprintf(stderr, "%s: could not reset hardware.\n", progname);
		exit(EXIT_FAILURE);
	}

	fin = fopen(filename, "r");
	if (fin == NULL) {
		fprintf(stderr, "%s: could not reopen config file\n", progname);
		if (curr_driver->deinit_func)
			curr_driver->deinit_func();
		exit(EXIT_FAILURE);
	}
	remotes = read_config(fin, filename);
	fclose(fin);
	if (remotes == NULL) {
		fprintf(stderr, "%s: config file contains no valid remote control definition\n", progname);
		fprintf(stderr, "%s: this shouldn't ever happen!\n", progname);
		if (curr_driver->deinit_func)
			curr_driver->deinit_func();
		exit(EXIT_FAILURE);
	}
	if (remotes == (void *)-1) {
		fprintf(stderr, "%s: reading of config file failed\n", progname);
		fprintf(stderr, "%s: this shouldn't ever happen!\n", progname);
		if (curr_driver->deinit_func)
			curr_driver->deinit_func();
		exit(EXIT_FAILURE);
	}

	if (!has_toggle_bit_mask(remotes)) {
		if (!using_template && strcmp(curr_driver->name, "devinput") != 0)
			get_toggle_bit_mask(remotes);
	} else {
		set_toggle_bit_mask(remotes, remotes->toggle_bit_mask);
	}
	if (curr_driver->deinit_func)
		curr_driver->deinit_func();
	get_pre_data(remotes);
	get_post_data(remotes);

	/* write final config file */
	fout = fopen(filename, "w");
	if (fout == NULL) {
		fprintf(stderr, "%s: could not open final config file \"%s\"\n",
			progname, filename);
		perror(progname);
		free_config(remotes);
		return (EXIT_FAILURE);
	}
	fprint_copyright(fout);
	fprint_remotes(fout, remotes, commandline);
	free_config(remotes);
	printf("Successfully written config file %s.\n", filename);
	return (EXIT_SUCCESS);
}

void flushhw(void)
{
	size_t size = 1;
	char buffer[sizeof(ir_code)];

	switch (curr_driver->rec_mode) {
	case LIRC_MODE_MODE2:
		while (availabledata())
			curr_driver->readdata(0);
		return;
	case LIRC_MODE_LIRCCODE:
		size = curr_driver->code_length / CHAR_BIT;
		if (curr_driver->code_length % CHAR_BIT)
			size++;
		break;
	}
	while (read(curr_driver->fd, buffer, size) == size) ;
}

int resethw(void)
{
	int flags;

	if (curr_driver->deinit_func)
		curr_driver->deinit_func();
	if (curr_driver->init_func) {
		if (!curr_driver->init_func())
			return (0);
	}
	flags = fcntl(curr_driver->fd, F_GETFL, 0);
	if (flags == -1 || fcntl(curr_driver->fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		if (curr_driver->deinit_func)
			curr_driver->deinit_func();
		return (0);
	}
	return (1);
}

static int mywaitfordata(__u32 maxusec)
{
	fd_set fds;
	int ret;
	struct timeval tv;

	while (1) {
		FD_ZERO(&fds);
		FD_SET(curr_driver->fd, &fds);
		do {
			do {
				if (maxusec > 0) {
					tv.tv_sec = maxusec / 1000000;
					tv.tv_usec = maxusec % 1000000;
					ret = select(curr_driver->fd + 1, &fds, NULL, NULL, &tv);
					if (ret == 0)
						return (0);
				} else {
					ret = select(curr_driver->fd + 1, &fds, NULL, NULL, NULL);
				}
			}
			while (ret == -1 && errno == EINTR);
			if (ret == -1) {
				logprintf(LIRC_ERROR, "select() failed\n");
				logperror(LIRC_ERROR, NULL);
				continue;
			}
		}
		while (ret == -1);

		if (FD_ISSET(curr_driver->fd, &fds)) {
			/* we will read later */
			return (1);
		}
	}
}

int availabledata(void)
{
	fd_set fds;
	int ret;
	struct timeval tv;

	FD_ZERO(&fds);
	FD_SET(curr_driver->fd, &fds);
	do {
		do {
			tv.tv_sec = 0;
			tv.tv_usec = 0;
			ret = select(curr_driver->fd + 1, &fds, NULL, NULL, &tv);
		}
		while (ret == -1 && errno == EINTR);
		if (ret == -1) {
			logprintf(LIRC_ERROR, "select() failed\n");
			logperror(LIRC_ERROR, NULL);
			continue;
		}
	}
	while (ret == -1);

	if (FD_ISSET(curr_driver->fd, &fds)) {
		return (1);
	}
	return (0);
}

int get_toggle_bit_mask(struct ir_remote *remote)
{
	struct decode_ctx_t decode_ctx;
	int retval = EXIT_SUCCESS;
	int retries, flag, success;
	ir_code first, last;
	int seq, repeats;
	int found;

	struct ir_ncode *codes;
	if (remote->codes) {
		codes = remote->codes;
		while (codes->name != NULL) {
			if (codes->next) {
				/* asume no toggle bit mask when key
				   sequences are used */
				return 1;
			}
			codes++;
		}
	}

	printf("Checking for toggle bit mask.\n");
	printf("Please press an arbitrary button repeatedly as fast as possible.\n"
	       "Make sure you keep pressing the SAME button and that you DON'T HOLD\n" "the button down!.\n"
	       "If you can't see any dots appear, then wait a bit between button presses.\n" "\n"
	       "Press RETURN to continue.");
	fflush(stdout);
	getchar();

	retries = 30;
	flag = success = 0;
	first = 0;
	last = 0;
	seq = repeats = 0;
	found = 0;
	while (availabledata()) {
		curr_driver->rec_func(NULL);
	}
	while (retval == EXIT_SUCCESS && retries > 0) {
		if (!mywaitfordata(10000000)) {
			printf("%s: no data for 10 secs, aborting\n", progname);
			retval = EXIT_FAILURE;
			break;
		}
		curr_driver->rec_func(remote);
		if (is_rc6(remote) && remote->rc6_mask == 0) {
			int i;
			ir_code mask;

			for (i = 0, mask = 1; i < remote->bits; i++, mask <<= 1) {
				remote->rc6_mask = mask;
				success = curr_driver->decode_func(remote, &decode_ctx);
				if (success) {
					remote->min_remaining_gap = decode_ctx.min_remaining_gap;
					remote->max_remaining_gap = decode_ctx.max_remaining_gap;
					break;
				}
			}
			if (success == 0)
				remote->rc6_mask = 0;
		} else {
			success = curr_driver->decode_func(remote, &decode_ctx);
			if (success) {
				remote->min_remaining_gap = decode_ctx.min_remaining_gap;
				remote->max_remaining_gap = decode_ctx.max_remaining_gap;
			}
		}
		if (success) {
			if (flag == 0) {
				flag = 1;
				first = decode_ctx.code;
			} else if (!decode_ctx.repeat_flag || decode_ctx.code != last) {
				seq++;
				if (!found && first ^ decode_ctx.code) {
					set_toggle_bit_mask(remote, first ^ decode_ctx.code);
					found = 1;
				}
				printf(".");
				fflush(stdout);
				retries--;
			} else {
				repeats++;
			}
			last = decode_ctx.code;
		} else {
			retries--;
		}
	}
	if (!found) {
		printf("\nNo toggle bit mask found.\n");
	} else {
		if (remote->toggle_bit_mask > 0) {
			printf("\nToggle bit mask is 0x%llx.\n", (__u64) remote->toggle_bit_mask);
		} else if (remote->toggle_mask != 0) {
			printf("\nToggle mask found.\n");
		}
	}
	if (seq > 0)
		remote->min_repeat = repeats / seq;
	logprintf(LIRC_DEBUG, "min_repeat=%d\n", remote->min_repeat);
	return (found);
}

void set_toggle_bit_mask(struct ir_remote *remote, ir_code xor)
{
	ir_code mask;
	struct ir_ncode *codes;
	int bits;

	if (!remote->codes)
		return;

	bits = bit_count(remote);
	mask = ((ir_code) 1) << (bits - 1);
	while (mask) {
		if (mask == xor)
			break;
		mask = mask >> 1;
	}
	if (mask) {
		remote->toggle_bit_mask = xor;

		codes = remote->codes;
		while (codes->name != NULL) {
			codes->code &= ~xor;
			codes++;
		}
	}
	/* Sharp, Denon and some others use a toggle_mask */
	else if (bits == 15 && xor == 0x3ff) {
		remote->toggle_mask = xor;
	} else {
		remote->toggle_bit_mask = xor;
	}
}

void get_pre_data(struct ir_remote *remote)
{
	struct ir_ncode *codes;
	ir_code mask, last;
	int count, i;

	if (remote->bits == 0)
		return;

	mask = (-1);
	codes = remote->codes;
	if (codes->name == NULL)
		return;		/* at least 2 codes needed */
	last = codes->code;
	codes++;
	if (codes->name == NULL)
		return;		/* at least 2 codes needed */
	while (codes->name != NULL) {
		struct ir_code_node *loop;

		mask &= ~(last ^ codes->code);
		last = codes->code;
		for (loop = codes->next; loop != NULL; loop = loop->next) {
			mask &= ~(last ^ loop->code);
			last = loop->code;
		}

		codes++;
	}
	count = 0;
	while (mask & 0x8000000000000000LL) {
		count++;
		mask = mask << 1;
	}
	count -= sizeof(ir_code) * CHAR_BIT - remote->bits;

	/* only "even" numbers should go to pre/post data */
	if (count % 8 && (remote->bits - count) % 8) {
		count -= count % 8;
	}
	if (count > 0) {
		mask = 0;
		for (i = 0; i < count; i++) {
			mask = mask << 1;
			mask |= 1;
		}
		remote->bits -= count;
		mask = mask << (remote->bits);
		remote->pre_data_bits = count;
		remote->pre_data = (last & mask) >> (remote->bits);

		codes = remote->codes;
		while (codes->name != NULL) {
			struct ir_code_node *loop;

			codes->code &= ~mask;
			for (loop = codes->next; loop != NULL; loop = loop->next) {
				loop->code &= ~mask;
			}
			codes++;
		}
	}
}

void get_post_data(struct ir_remote *remote)
{
	struct ir_ncode *codes;
	ir_code mask, last;
	int count, i;

	if (remote->bits == 0)
		return;

	mask = (-1);
	codes = remote->codes;
	if (codes->name == NULL)
		return;		/* at least 2 codes needed */
	last = codes->code;
	codes++;
	if (codes->name == NULL)
		return;		/* at least 2 codes needed */
	while (codes->name != NULL) {
		struct ir_code_node *loop;

		mask &= ~(last ^ codes->code);
		last = codes->code;
		for (loop = codes->next; loop != NULL; loop = loop->next) {
			mask &= ~(last ^ loop->code);
			last = loop->code;
		}
		codes++;
	}
	count = 0;
	while (mask & 0x1) {
		count++;
		mask = mask >> 1;
	}
	/* only "even" numbers should go to pre/post data */
	if (count % 8 && (remote->bits - count) % 8) {
		count -= count % 8;
	}
	if (count > 0) {
		mask = 0;
		for (i = 0; i < count; i++) {
			mask = mask << 1;
			mask |= 1;
		}
		remote->bits -= count;
		remote->post_data_bits = count;
		remote->post_data = last & mask;

		codes = remote->codes;
		while (codes->name != NULL) {
			struct ir_code_node *loop;

			codes->code = codes->code >> count;
			for (loop = codes->next; loop != NULL; loop = loop->next) {
				loop->code = loop->code >> count;
			}
			codes++;
		}
	}
}

void for_each_remote(struct ir_remote *remotes, remote_func func)
{
	struct ir_remote *remote;

	remote = remotes;
	while (remote != NULL) {
		func(remote);
		remote = remote->next;
	}
}


void analyse_remote(struct ir_remote *raw_data)
{
	const char* const commandline = "Created using --analyse (-a)";
	struct ir_ncode *codes;
	struct decode_ctx_t decode_ctx;
	int code;
	int code2;
	struct ir_ncode *new_codes;
	size_t new_codes_count = 100;
	int new_index = 0;
	int ret;

	if (!is_raw(raw_data)) {
		fprintf(stderr, "%s: remote %s not in raw mode, ignoring\n", progname, raw_data->name);
		return;
	}
	aeps = raw_data->aeps;
	eps = raw_data->eps;
	emulation_data = raw_data;
	next_code = NULL;
	current_code = NULL;
	current_index = 0;
	memset(&remote, 0, sizeof(remote));
	get_lengths(&remote, 0, 0 /* not interactive */ );

	if (is_rc6(&remote) && remote.bits >= 5) {
		/* have to assume something as it's very difficult to
		   extract the rc6_mask from the data that we have */
		remote.rc6_mask = ((ir_code) 0x1ll) << (remote.bits - 5);
	}

	remote.name = raw_data->name;
	remote.freq = raw_data->freq;

	new_codes = malloc(new_codes_count * sizeof(*new_codes));
	if (new_codes == NULL) {
		fprintf(stderr, "%s: out of memory\n", progname);
		return;
	}
	memset(new_codes, 0, new_codes_count * sizeof(*new_codes));
	codes = raw_data->codes;
	while (codes->name != NULL) {
		//printf("decoding %s\n", codes->name);
		current_code = NULL;
		current_index = 0;
		next_code = codes;

		rec_buffer_init();

		ret = receive_decode(&remote, &decode_ctx);
		if (!ret) {
			fprintf(stderr, "%s: decoding of %s failed\n", progname, codes->name);
		} else {
			if (new_index + 1 >= new_codes_count) {
				struct ir_ncode *renew_codes;

				new_codes_count *= 2;
				renew_codes = realloc(new_codes, new_codes_count * sizeof(*new_codes));
				if (renew_codes == NULL) {
					fprintf(stderr, "%s: out of memory\n", progname);
					free(new_codes);
					return;
				}
				memset(&new_codes[new_codes_count / 2], 0, new_codes_count / 2 * sizeof(*new_codes));
				new_codes = renew_codes;
			}

			rec_buffer_clear();
			code = decode_ctx.code;
			ret = receive_decode(&remote, &decode_ctx);
			code2 = decode_ctx.code;
			decode_ctx.code = code;
			if (ret && code2 != decode_ctx.code) {
				new_codes[new_index].next = malloc(sizeof(*(new_codes[new_index].next)));
				if (new_codes[new_index].next) {
					memset(new_codes[new_index].next, 0, sizeof(*(new_codes[new_index].next)));
					new_codes[new_index].next->code = code2;
				}
			}
			new_codes[new_index].name = codes->name;
			new_codes[new_index].code = decode_ctx.code;
			new_index++;
		}
		codes++;
	}
	new_codes[new_index].name = NULL;
	remote.codes = new_codes;
	fprint_remotes(stdout, &remote, commandline);
	remote.codes = NULL;
	free(new_codes);
}

void remove_pre_data(struct ir_remote *remote)
{
	struct ir_ncode *codes;

	if (remote->pre_data_bits == 0 || remote->pre_p != 0 || remote->pre_s != 0) {
		remote = remote->next;
		return;
	}

	codes = remote->codes;
	while (codes->name != NULL) {
		struct ir_code_node *loop;

		codes->code |= remote->pre_data << remote->bits;
		for (loop = codes->next; loop != NULL; loop = loop->next) {
			loop->code |= remote->pre_data << remote->bits;
		}
		codes++;
	}
	remote->bits += remote->pre_data_bits;
	remote->pre_data = 0;
	remote->pre_data_bits = 0;
}

void remove_post_data(struct ir_remote *remote)
{
	struct ir_ncode *codes;

	if (remote->post_data_bits == 0) {
		remote = remote->next;
		return;
	}

	codes = remote->codes;
	while (codes->name != NULL) {
		struct ir_code_node *loop;

		codes->code <<= remote->post_data_bits;
		codes->code |= remote->post_data;
		for (loop = codes->next; loop != NULL; loop = loop->next) {
			loop->code <<= remote->post_data_bits;
			loop->code |= remote->post_data;
		}
		codes++;
	}
	remote->bits += remote->post_data_bits;
	remote->post_data = 0;
	remote->post_data_bits = 0;
}

void invert_data(struct ir_remote *remote)
{
	struct ir_ncode *codes;
	ir_code mask;
	lirc_t p, s;

	/* swap one, zero */
	p = remote->pone;
	s = remote->sone;
	remote->pone = remote->pzero;
	remote->sone = remote->szero;
	remote->pzero = p;
	remote->szero = s;

	/* invert pre_data */
	if (has_pre(remote)) {
		mask = gen_mask(remote->pre_data_bits);
		remote->pre_data ^= mask;
	}
	/* invert post_data */
	if (has_post(remote)) {
		mask = gen_mask(remote->post_data_bits);
		remote->post_data ^= mask;
	}

	if (remote->bits == 0) {
		remote = remote->next;
		return;
	}

	/* invert codes */
	mask = gen_mask(remote->bits);
	codes = remote->codes;
	while (codes->name != NULL) {
		struct ir_code_node *loop;

		codes->code ^= mask;
		for (loop = codes->next; loop != NULL; loop = loop->next) {
			loop->code ^= mask;
		}
		codes++;
	}
}

void remove_trail(struct ir_remote *remote)
{
	int extra_bit;

	if (!is_space_enc(remote))
		return;

	if (remote->ptrail == 0)
		return;

	if (expect(remote, remote->pone, remote->pzero) || expect(remote, remote->pzero, remote->pone))
		return;

	if (!(expect(remote, remote->sone, remote->szero) && expect(remote, remote->szero, remote->sone)))
		return;

	if (expect(remote, remote->ptrail, remote->pone)) {
		extra_bit = 1;
	} else if (expect(remote, remote->ptrail, remote->pzero)) {
		extra_bit = 0;
	} else {
		return;
	}

	remote->post_data_bits++;
	remote->post_data <<= 1;
	remote->post_data |= extra_bit;
	remote->ptrail = 0;
}

/* analyse stuff */

struct lengths {
	unsigned int count;
	lirc_t sum, upper_bound, lower_bound, min, max;
	struct lengths *next;
};

enum analyse_mode { MODE_GAP, MODE_HAVE_GAP };

struct lengths *first_space = NULL, *first_pulse = NULL;
struct lengths *first_sum = NULL, *first_gap = NULL, *first_repeat_gap = NULL;
struct lengths *first_signal_length = NULL;
struct lengths *first_headerp = NULL, *first_headers = NULL;
struct lengths *first_1lead = NULL, *first_3lead = NULL, *first_trail = NULL;
struct lengths *first_repeatp = NULL, *first_repeats = NULL;
__u32 lengths[MAX_SIGNALS];
__u32 first_length, first_lengths, second_lengths;
unsigned int count, count_spaces, count_3repeats, count_5repeats, count_signals;

lirc_t calc_signal(struct lengths *len)
{
	return ((lirc_t) (len->sum / len->count));
}

int get_lengths(struct ir_remote *remote, int force, int interactive)
{
	int retval;
	lirc_t data, average, maxspace, sum, remaining_gap, header;
	enum analyse_mode mode = MODE_GAP;
	int first_signal;

	if (interactive) {
		printf("Now start pressing buttons on your remote control.\n\n");
		printf("It is very important that you press many different buttons and hold them\n"
		       "down for approximately one second. Each button should generate at least one\n"
		       "dot but in no case more than ten dots of output.\n"
		       "Don't stop pressing buttons until two lines of dots (2x80) have been\n" "generated.\n\n");
		printf("Press RETURN now to start recording.");
		fflush(stdout);
		getchar();
		flushhw();
	}
	retval = 1;
	average = 0;
	maxspace = 0;
	sum = 0;
	count = 0;
	count_spaces = 0;
	count_3repeats = 0;
	count_5repeats = 0;
	count_signals = 0;
	first_signal = -1;
	header = 0;
	first_length = 0;
	first_lengths = 0;
	second_lengths = 0;
	memset(lengths, 0, sizeof(lengths));
	while (1) {
		data = curr_driver->readdata(10000000);
		if (!data) {
			fprintf(stderr, "%s: no data for 10 secs, aborting\n", progname);
			retval = 0;
			break;
		}
		count++;
		if (mode == MODE_GAP) {
			sum += data & PULSE_MASK;
			if (average == 0 && is_space(data)) {
				if (data > 100000) {
					sum = 0;
					continue;
				}
				average = data;
				maxspace = data;
			} else if (is_space(data)) {
				if (data > MIN_GAP || data > 100 * average ||
				    /* this MUST be a gap */
				    (data >= 5000 && count_spaces > 10 && data > 5 * average) || (data < 5000
												  && count_spaces > 10
												  && data >
												  5 * maxspace / 2)
				    /* || Echostar
				       (count_spaces>20 && data>9*maxspace/10) */
				    )
					/* this should be a gap */
				{
					struct lengths *scan;
					int maxcount;
					static int lastmaxcount = 0;
					int i;

					add_length(&first_sum, sum);
					merge_lengths(first_sum);
					add_length(&first_gap, data);
					merge_lengths(first_gap);
					sum = 0;
					count_spaces = 0;
					average = 0;
					maxspace = 0;

					maxcount = 0;
					scan = first_sum;
					while (scan) {
						maxcount = max(maxcount, scan->count);
						if (scan->count > SAMPLES) {
							remote->gap = calc_signal(scan);
							remote->flags |= CONST_LENGTH;
							i_printf(interactive, "\nFound const length: %lu\n",
								 (__u32) remote->gap);
							break;
						}
						scan = scan->next;
					}
					if (scan == NULL) {
						scan = first_gap;
						while (scan) {
							maxcount = max(maxcount, scan->count);
							if (scan->count > SAMPLES) {
								remote->gap = calc_signal(scan);
								i_printf(interactive, "\nFound gap: %lu\n",
									 (__u32) remote->gap);
								break;
							}
							scan = scan->next;
						}
					}
					if (scan != NULL) {
						i_printf(interactive,
							 "Please keep on pressing buttons like described above.\n");
						mode = MODE_HAVE_GAP;
						sum = 0;
						count = 0;
						remaining_gap =
						    is_const(remote) ? (remote->gap >
									data ? remote->gap -
									data : 0) : (has_repeat_gap(remote) ? remote->
										     repeat_gap : remote->gap);
						if (force) {
							retval = 0;
							break;
						}
						continue;
					}

					if (interactive) {
						for (i = maxcount - lastmaxcount; i > 0; i--) {
							printf(".");
							fflush(stdout);
						}
					}
					lastmaxcount = maxcount;

					continue;
				}
				average = (average * count_spaces + data)
				    / (count_spaces + 1);
				count_spaces++;
				if (data > maxspace) {
					maxspace = data;
				}
			}
			if (count > SAMPLES * MAX_SIGNALS * 2) {
				fprintf(stderr, "\n%s: could not find gap.\n", progname);
				retval = 0;
				break;
			}
		} else if (mode == MODE_HAVE_GAP) {
			if (count <= MAX_SIGNALS) {
				signals[count - 1] = data & PULSE_MASK;
			} else {
				fprintf(stderr, "%s: signal too long\n", progname);
				retval = 0;
				break;
			}
			if (is_const(remote)) {
				remaining_gap = remote->gap > sum ? remote->gap - sum : 0;
			} else {
				remaining_gap = remote->gap;
			}
			sum += data & PULSE_MASK;

			if (count > 2
			    && ((data & PULSE_MASK) >= remaining_gap * (100 - eps) / 100
				|| (data & PULSE_MASK) >= remaining_gap - aeps)) {
				if (is_space(data)) {
					/* signal complete */
					if (count == 4) {
						count_3repeats++;
						add_length(&first_repeatp, signals[0]);
						merge_lengths(first_repeatp);
						add_length(&first_repeats, signals[1]);
						merge_lengths(first_repeats);
						add_length(&first_trail, signals[2]);
						merge_lengths(first_trail);
						add_length(&first_repeat_gap, signals[3]);
						merge_lengths(first_repeat_gap);
					} else if (count == 6) {
						count_5repeats++;
						add_length(&first_headerp, signals[0]);
						merge_lengths(first_headerp);
						add_length(&first_headers, signals[1]);
						merge_lengths(first_headers);
						add_length(&first_repeatp, signals[2]);
						merge_lengths(first_repeatp);
						add_length(&first_repeats, signals[3]);
						merge_lengths(first_repeats);
						add_length(&first_trail, signals[4]);
						merge_lengths(first_trail);
						add_length(&first_repeat_gap, signals[5]);
						merge_lengths(first_repeat_gap);
					} else if (count > 6) {
						int i;

						if (interactive) {
							printf(".");
							fflush(stdout);
						}
						count_signals++;
						add_length(&first_1lead, signals[0]);
						merge_lengths(first_1lead);
						for (i = 2; i < count - 2; i++) {
							if (i % 2) {
								add_length(&first_space, signals[i]);
								merge_lengths(first_space);
							} else {
								add_length(&first_pulse, signals[i]);
								merge_lengths(first_pulse);
							}
						}
						add_length(&first_trail, signals[count - 2]);
						merge_lengths(first_trail);
						lengths[count - 2]++;
						add_length(&first_signal_length, sum - data);
						merge_lengths(first_signal_length);
						if (first_signal == 1
						    || (first_length > 2 && first_length - 2 != count - 2)) {
							add_length(&first_3lead, signals[2]);
							merge_lengths(first_3lead);
							add_length(&first_headerp, signals[0]);
							merge_lengths(first_headerp);
							add_length(&first_headers, signals[1]);
							merge_lengths(first_headers);
						}
						if (first_signal == 1) {
							first_lengths++;
							first_length = count - 2;
							header = signals[0] + signals[1];
						} else if (first_signal == 0 && first_length - 2 == count - 2) {
							lengths[count - 2]--;
							lengths[count - 2 + 2]++;
							second_lengths++;
						}
					}
					count = 0;
					sum = 0;
				}
#if 0
				/* such long pulses may appear with
				   crappy hardware (receiver? / remote?)
				 */
				else {
					fprintf(stderr, "%s: wrong gap\n", progname);
					remote->gap = 0;
					retval = 0;
					break;
				}
#endif
				if (count_signals >= SAMPLES) {
					i_printf(interactive, "\n");
					get_scheme(remote, interactive);
					if (!get_header_length(remote, interactive)
					    || !get_trail_length(remote, interactive)
					    || !get_lead_length(remote, interactive)
					    || !get_repeat_length(remote, interactive)
					    || !get_data_length(remote, interactive)) {
						retval = 0;
					}
					break;
				}
				if ((data & PULSE_MASK) <= (remaining_gap + header) * (100 + eps) / 100
				    || (data & PULSE_MASK) <= (remaining_gap + header) + aeps) {
					first_signal = 0;
					header = 0;
				} else {
					first_signal = 1;
				}
			}
		}
	}
	free_lengths(&first_space);
	free_lengths(&first_pulse);
	free_lengths(&first_sum);
	free_lengths(&first_gap);
	free_lengths(&first_repeat_gap);
	free_lengths(&first_signal_length);
	free_lengths(&first_headerp);
	free_lengths(&first_headers);
	free_lengths(&first_1lead);
	free_lengths(&first_3lead);
	free_lengths(&first_trail);
	free_lengths(&first_repeatp);
	free_lengths(&first_repeats);
	return (retval);
}

/* handle lengths */

struct lengths *new_length(lirc_t length)
{
	struct lengths *l;

	l = malloc(sizeof(struct lengths));
	if (l == NULL)
		return (NULL);
	l->count = 1;
	l->sum = length;
	l->lower_bound = length / 100 * 100;
	l->upper_bound = length / 100 * 100 + 99;
	l->min = l->max = length;
	l->next = NULL;
	return (l);
}

int add_length(struct lengths **first, lirc_t length)
{
	struct lengths *l, *last;

	if (*first == NULL) {
		*first = new_length(length);
		if (*first == NULL)
			return (0);
		return (1);
	}
	l = *first;
	while (l != NULL) {
		if (l->lower_bound <= length && length <= l->upper_bound) {
			l->count++;
			l->sum += length;
			l->min = min(l->min, length);
			l->max = max(l->max, length);
			return (1);
		}
		last = l;
		l = l->next;
	}
	last->next = new_length(length);
	if (last->next == NULL)
		return (0);
	return (1);
}

void free_lengths(struct lengths **firstp)
{
	struct lengths *first, *next;

	first = *firstp;
	if (first == NULL)
		return;
	while (first != NULL) {
		next = first->next;
		free(first);
		first = next;
	}
	*firstp = NULL;
}

void merge_lengths(struct lengths *first)
{
	struct lengths *l, *inner, *last;
	__u32 new_sum;
	int new_count;

	l = first;
	while (l != NULL) {
		last = l;
		inner = l->next;
		while (inner != NULL) {
			new_sum = l->sum + inner->sum;
			new_count = l->count + inner->count;

			if ((l->max <= new_sum / new_count + aeps && l->min + aeps >= new_sum / new_count
			     && inner->max <= new_sum / new_count + aeps && inner->min + aeps >= new_sum / new_count)
			    || (l->max <= new_sum / new_count * (100 + eps)
				&& l->min >= new_sum / new_count * (100 - eps)
				&& inner->max <= new_sum / new_count * (100 + eps)
				&& inner->min >= new_sum / new_count * (100 - eps))) {
				l->sum = new_sum;
				l->count = new_count;
				l->upper_bound = max(l->upper_bound, inner->upper_bound);
				l->lower_bound = min(l->lower_bound, inner->lower_bound);
				l->min = min(l->min, inner->min);
				l->max = max(l->max, inner->max);

				last->next = inner->next;
				free(inner);
				inner = last;
			}
			last = inner;
			inner = inner->next;
		}
		l = l->next;
	}
	if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
		l = first;
		while (l != NULL) {
			printf("%d x %u [%u,%u]\n",
			       l->count, (__u32) calc_signal(l), (__u32) l->min, (__u32) l->max);
			l = l->next;
		}
	}
}

void get_scheme(struct ir_remote *remote, int interactive)
{
	unsigned int i, length = 0, sum = 0;

	for (i = 1; i < MAX_SIGNALS; i++) {
		if (lengths[i] > lengths[length]) {
			length = i;
		}
		sum += lengths[i];
		if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
			if (lengths[i] > 0)
				printf("%u: %u\n", i, lengths[i]);
		}
	}
	if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
		printf("get_scheme(): sum: %u length: %u signals: %u\n"
		       "first_lengths: %u second_lengths: %u\n",
			sum, length + 1, lengths[length], first_lengths, second_lengths);
	}
	/* FIXME !!! this heuristic is too bad */
	if (lengths[length] >= TH_SPACE_ENC * sum / 100) {
		length++;
		i_printf(interactive, "Space/pulse encoded remote control found.\n");
		i_printf(interactive, "Signal length is %u.\n", length);
		/* this is not yet the
		   number of bits */
		remote->bits = length;
		set_protocol(remote, SPACE_ENC);
		return;
	} else {
		struct lengths *maxp, *max2p, *maxs, *max2s;

		maxp = get_max_length(first_pulse, NULL);
		unlink_length(&first_pulse, maxp);
		if (first_pulse == NULL) {
			first_pulse = maxp;
		} else {
			max2p = get_max_length(first_pulse, NULL);
			maxp->next = first_pulse;
			first_pulse = maxp;

			maxs = get_max_length(first_space, NULL);
			unlink_length(&first_space, maxs);
			if (first_space == NULL) {
				first_space = maxs;
			} else {
				max2s = get_max_length(first_space, NULL);
				maxs->next = first_space;
				first_space = maxs;

				maxs = get_max_length(first_space, NULL);

				if (length > 20
				    && (calc_signal(maxp) < TH_RC6_SIGNAL || calc_signal(max2p) < TH_RC6_SIGNAL)
				    && (calc_signal(maxs) < TH_RC6_SIGNAL || calc_signal(max2s) < TH_RC6_SIGNAL)) {
					i_printf(interactive, "RC-6 remote control found.\n");
					set_protocol(remote, RC6);
				} else {
					i_printf(interactive, "RC-5 remote control found.\n");
					set_protocol(remote, RC5);
				}
				return;
			}
		}
	}
	length++;
	i_printf(interactive, "Suspicious data length: %u.\n", length);
	/* this is not yet the number of bits */
	remote->bits = length;
	set_protocol(remote, SPACE_ENC);
}

struct lengths *get_max_length(struct lengths *first, unsigned int *sump)
{
	unsigned int sum;
	struct lengths *scan, *max_length;

	if (first == NULL)
		return (NULL);
	max_length = first;
	sum = first->count;

	if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
		if (first->count > 0)
			printf("%u x %u\n", first->count, (__u32) calc_signal(first));
	}
	scan = first->next;
	while (scan) {
		if (scan->count > max_length->count) {
			max_length = scan;
		}
		sum += scan->count;
		if (lirc_log_is_enabled_for(LIRC_DEBUG) && scan->count > 0) {
			printf("%u x %u\n", scan->count, (__u32) calc_signal(scan));
		}
		scan = scan->next;
	}
	if (sump != NULL)
		*sump = sum;
	return (max_length);
}

int get_trail_length(struct ir_remote *remote, int interactive)
{
	unsigned int sum = 0, max_count;
	struct lengths *max_length;

	if (is_biphase(remote))
		return (1);

	max_length = get_max_length(first_trail, &sum);
	max_count = max_length->count;
	if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
		logprintf(LIRC_DEBUG, "get_trail_length(): sum: %u, max_count %u\n",
			  sum, max_count);
	}
	if (max_count >= sum * TH_TRAIL / 100) {
		i_printf(interactive, "Found trail pulse: %lu\n", (__u32) calc_signal(max_length));
		remote->ptrail = calc_signal(max_length);
		return (1);
	}
	i_printf(interactive, "No trail pulse found.\n");
	return (1);
}

int get_lead_length(struct ir_remote *remote, int interactive)
{
	unsigned int sum = 0, max_count;
	struct lengths *first_lead, *max_length, *max2_length;
	lirc_t a, b, swap;

	if (!is_biphase(remote) || has_header(remote))
		return (1);
	if (is_rc6(remote))
		return (1);

	first_lead = has_header(remote) ? first_3lead : first_1lead;
	max_length = get_max_length(first_lead, &sum);
	max_count = max_length->count;
	if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
		printf("get_lead_length(): sum: %u, max_count %u\n", sum, max_count);
	}
	if (max_count >= sum * TH_LEAD / 100) {
		i_printf(interactive, "Found lead pulse: %lu\n", (__u32) calc_signal(max_length));
		remote->plead = calc_signal(max_length);
		return (1);
	}
	unlink_length(&first_lead, max_length);
	max2_length = get_max_length(first_lead, &sum);
	max_length->next = first_lead;
	first_lead = max_length;

	a = calc_signal(max_length);
	b = calc_signal(max2_length);
	if (a > b) {
		swap = a;
		a = b;
		b = swap;
	}
	if (abs(2 * a - b) < b * eps / 100 || abs(2 * a - b) < aeps) {
		i_printf(interactive, "Found hidden lead pulse: %lu\n", (__u32) a);
		remote->plead = a;
		return (1);
	}
	i_printf(interactive, "No lead pulse found.\n");
	return (1);
}

int get_header_length(struct ir_remote *remote, int interactive)
{
	unsigned int sum, max_count;
	lirc_t headerp, headers;
	struct lengths *max_plength, *max_slength;

	if (first_headerp != NULL) {
		max_plength = get_max_length(first_headerp, &sum);
		max_count = max_plength->count;
	} else {
		i_printf(interactive, "No header data.\n");
		return (1);
	}
	if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
		printf("get_header_length(): sum: %u, max_count %u\n", sum, max_count);
	}

	if (max_count >= sum * TH_HEADER / 100) {
		max_slength = get_max_length(first_headers, &sum);
		max_count = max_slength->count;
		if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
			printf("get_header_length(): sum: %u, max_count %u\n", sum, max_count);
		}
		if (max_count >= sum * TH_HEADER / 100) {
			headerp = calc_signal(max_plength);
			headers = calc_signal(max_slength);

			i_printf(interactive, "Found possible header: %lu %lu\n", (__u32) headerp, (__u32) headers);
			remote->phead = headerp;
			remote->shead = headers;
			if (first_lengths < second_lengths) {
				i_printf(interactive, "Header is not being repeated.\n");
				remote->flags |= NO_HEAD_REP;
			}
			return (1);
		}
	}
	i_printf(interactive, "No header found.\n");
	return (1);
}

int get_repeat_length(struct ir_remote *remote, int interactive)
{
	unsigned int sum = 0, max_count;
	lirc_t repeatp, repeats, repeat_gap;
	struct lengths *max_plength, *max_slength;

	if (!((count_3repeats > SAMPLES / 2 ? 1 : 0) ^ (count_5repeats > SAMPLES / 2 ? 1 : 0))) {
		if (count_3repeats > SAMPLES / 2 || count_5repeats > SAMPLES / 2) {
			printf("Repeat inconsitentcy.\n");
			return (0);
		}
		i_printf(interactive, "No repeat code found.\n");
		return (1);
	}

	max_plength = get_max_length(first_repeatp, &sum);
	max_count = max_plength->count;
	if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
		printf("get_repeat_length(): sum: %u, max_count %u\n", sum, max_count);
	}

	if (max_count >= sum * TH_REPEAT / 100) {
		max_slength = get_max_length(first_repeats, &sum);
		max_count = max_slength->count;
		if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
			printf("get_repeat_length(): sum: %u, max_count %u\n", sum, max_count);
		}
		if (max_count >= sum * TH_REPEAT / 100) {
			if (count_5repeats > count_3repeats && !has_header(remote)) {
				printf("Repeat code has header," " but no header found!\n");
				return (0);
			}
			if (count_5repeats > count_3repeats && has_header(remote)) {
				remote->flags |= REPEAT_HEADER;
			}
			repeatp = calc_signal(max_plength);
			repeats = calc_signal(max_slength);

			i_printf(interactive, "Found repeat code: %lu %lu\n", (__u32) repeatp, (__u32) repeats);
			remote->prepeat = repeatp;
			remote->srepeat = repeats;
			if (!(remote->flags & CONST_LENGTH)) {
				max_slength = get_max_length(first_repeat_gap, NULL);
				repeat_gap = calc_signal(max_slength);
				i_printf(interactive, "Found repeat gap: %lu\n", (__u32) repeat_gap);
				remote->repeat_gap = repeat_gap;

			}
			return (1);
		}
	}
	i_printf(interactive, "No repeat header found.\n");
	return (1);

}

void unlink_length(struct lengths **first, struct lengths *remove)
{
	struct lengths *last, *scan;

	if (remove == *first) {
		*first = remove->next;
		remove->next = NULL;
		return;
	} else {
		scan = (*first)->next;
		last = *first;
		while (scan) {
			if (scan == remove) {
				last->next = remove->next;
				remove->next = NULL;
				return;
			}
			last = scan;
			scan = scan->next;
		}
	}
	printf("unlink_length(): report this bug!\n");
}

int get_data_length(struct ir_remote *remote, int interactive)
{
	unsigned int sum = 0, max_count;
	lirc_t p1, p2, s1, s2;
	struct lengths *max_plength, *max_slength;
	struct lengths *max2_plength, *max2_slength;

	max_plength = get_max_length(first_pulse, &sum);
	max_count = max_plength->count;
	if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
		printf("get_data_length(): sum: %u, max_count %u\n", sum, max_count);
	}

	if (max_count >= sum * TH_IS_BIT / 100) {
		unlink_length(&first_pulse, max_plength);

		max2_plength = get_max_length(first_pulse, NULL);
		if (max2_plength != NULL) {
			if (max2_plength->count < max_count * TH_IS_BIT / 100)
				max2_plength = NULL;
		}
		if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
			printf("Pulse canditates: ");
			printf("%u x %u", max_plength->count, (__u32) calc_signal(max_plength));
			if (max2_plength)
				printf(", %u x %u", max2_plength->count, (__u32)
				       calc_signal(max2_plength));
			printf("\n");
		}

		max_slength = get_max_length(first_space, &sum);
		max_count = max_slength->count;
		if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
			printf("get_data_length(): sum: %u, max_count %u\n", sum, max_count);
		}
		if (max_count >= sum * TH_IS_BIT / 100) {
			unlink_length(&first_space, max_slength);

			max2_slength = get_max_length(first_space, NULL);
			if (max2_slength != NULL) {
				if (max2_slength->count < max_count * TH_IS_BIT / 100)
					max2_slength = NULL;
			}
			if (lirc_log_is_enabled_for(LIRC_DEBUG)) {
				if (max_count >= sum * TH_IS_BIT / 100) {
					printf("Space candidates: ");
					printf("%u x %u", max_slength->count,
					       (__u32) calc_signal(max_slength));
					if (max2_slength)
						printf(", %u x %u",
						       max2_slength->count,
						       (__u32) calc_signal(max2_slength));
					printf("\n");
				}
			}
			remote->eps = eps;
			remote->aeps = aeps;
			if (is_biphase(remote)) {
				if (max2_plength == NULL || max2_slength == NULL) {
					printf("Unknown encoding found.\n");
					return (0);
				}
				i_printf(interactive, "Signals are biphase encoded.\n");
				p1 = calc_signal(max_plength);
				p2 = calc_signal(max2_plength);
				s1 = calc_signal(max_slength);
				s2 = calc_signal(max2_slength);

				remote->pone = (min(p1, p2) + max(p1, p2) / 2) / 2;
				remote->sone = (min(s1, s2) + max(s1, s2) / 2) / 2;
				remote->pzero = remote->pone;
				remote->szero = remote->sone;
			} else {
				if (max2_plength == NULL && max2_slength == NULL) {
					printf("No encoding found.\n");
					return (0);
				}
				if (max2_plength && max2_slength) {
					printf("Unknown encoding found.\n");
					return (0);
				}
				p1 = calc_signal(max_plength);
				s1 = calc_signal(max_slength);
				if (max2_plength) {
					p2 = calc_signal(max2_plength);
					i_printf(interactive, "Signals are pulse encoded.\n");
					remote->pone = max(p1, p2);
					remote->sone = s1;
					remote->pzero = min(p1, p2);
					remote->szero = s1;
					if (expect(remote, remote->ptrail, p1) || expect(remote, remote->ptrail, p2)) {
						remote->ptrail = 0;
					}
				} else {
					s2 = calc_signal(max2_slength);
					i_printf(interactive, "Signals are space encoded.\n");
					remote->pone = p1;
					remote->sone = max(s1, s2);
					remote->pzero = p1;
					remote->szero = min(s1, s2);
				}
			}
			if (has_header(remote) && (!has_repeat(remote) || remote->flags & NO_HEAD_REP)
			    ) {
				if (!is_biphase(remote)
				    &&
				    ((expect(remote, remote->phead, remote->pone)
				      && expect(remote, remote->shead, remote->sone))
				     || (expect(remote, remote->phead, remote->pzero)
					 && expect(remote, remote->shead, remote->szero)))) {
					remote->phead = remote->shead = 0;
					remote->flags &= ~NO_HEAD_REP;
					i_printf(interactive, "Removed header.\n");
				}
				if (is_biphase(remote) && expect(remote, remote->shead, remote->sone)) {
					remote->plead = remote->phead;
					remote->phead = remote->shead = 0;
					remote->flags &= ~NO_HEAD_REP;
					i_printf(interactive, "Removed header.\n");
				}
			}
			if (is_biphase(remote)) {
				struct lengths *signal_length;
				lirc_t data_length;

				signal_length = get_max_length(first_signal_length, NULL);
				data_length =
				    calc_signal(signal_length) - remote->plead - remote->phead - remote->shead +
				    /* + 1/2 bit */
				    (remote->pone + remote->sone) / 2;
				remote->bits = data_length / (remote->pone + remote->sone);
				if (is_rc6(remote))
					remote->bits--;

			} else {
				remote->bits =
				    (remote->bits - (has_header(remote) ? 2 : 0) + 1 -
				     (remote->ptrail > 0 ? 2 : 0)) / 2;
			}
			i_printf(interactive, "Signal length is %d\n", remote->bits);
			free_lengths(&max_plength);
			free_lengths(&max_slength);
			return (1);
		}
		free_lengths(&max_plength);
	}
	printf("Could not find data lengths.\n");
	return (0);
}

int get_gap_length(struct ir_remote *remote)
{
	struct lengths *gaps = NULL;
	struct timeval start, end, last = { 0, 0 };
	int flag;
	struct lengths *scan;
	int maxcount, lastmaxcount;
	lirc_t gap;

	remote->eps = eps;
	remote->aeps = aeps;

	flag = 0;
	lastmaxcount = 0;
	printf("Hold down an arbitrary button.\n");
	while (1) {
		while (availabledata()) {
			curr_driver->rec_func(NULL);
		}
		if (!mywaitfordata(10000000)) {
			free_lengths(&gaps);
			return (0);
		}
		gettimeofday(&start, NULL);
		while (availabledata()) {
			curr_driver->rec_func(NULL);
		}
		gettimeofday(&end, NULL);
		if (flag) {
			gap = time_elapsed(&last, &start);
			add_length(&gaps, gap);
			merge_lengths(gaps);
			maxcount = 0;
			scan = gaps;
			while (scan) {
				maxcount = max(maxcount, scan->count);
				if (scan->count > SAMPLES) {
					remote->gap = calc_signal(scan);
					printf("\nFound gap length: %u\n", (__u32) remote->gap);
					free_lengths(&gaps);
					return (1);
				}
				scan = scan->next;
			}
			if (maxcount > lastmaxcount) {
				lastmaxcount = maxcount;
				printf(".");
				fflush(stdout);
			}
		} else {
			flag = 1;
		}
		last = end;
	}
	return (1);
}

void fprint_copyright(FILE * fout)
{
	fprintf(fout, "\n"
		"# Please take the time to finish this file as described in\n"
		"# https://sourceforge.net/p/lirc-remotes/wiki/Checklist/\n"
		"# and make it available to others by sending it to \n"
		"# <lirc@bartelmus.de>\n");
}
