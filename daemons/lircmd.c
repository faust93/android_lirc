
/****************************************************************************
 ** lircmd.c ****************************************************************
 ****************************************************************************
 *
 * lircmd - LIRC Mouse Daemon
 *
 * Copyright (C) 1998 Christoph Bartelmus <lirc@bartelmus.de>
 *
 * Wheel support based on lirc-imps2 by
 * Ryan Gammon <rggammon@engmail.uwaterloo.ca>
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#if defined(__linux__)
#include <linux/input.h>
#include <linux/uinput.h>
#else
#include <stdint.h>
typedef int8_t   __s8;
typedef uint8_t  __u8;
typedef int16_t  __s16;
typedef uint16_t __u16;
typedef int32_t  __s32;
typedef uint32_t __u32;
typedef int64_t  __s64;
typedef uint64_t __u64;
#endif

#include "lirc_options.h"
#include "lirc_log.h"

#define CLICK_DELAY 50000	/* usecs */
#define WHITE_SPACE " \t"
#define ALL ((char *) (-1))
#define CIRCLE 10

#define BUTTONS 3		/* 3 buttons supported */

/* buttons chosen to match MouseSystem protocol*/
#define BUTTON1 0x04
#define BUTTON2 0x02
#define BUTTON3 0x01

#define MAP_BUTTON1 0
#define MAP_BUTTON2 1
#define MAP_BUTTON3 2

static int uinputfd = -1;
static int useuinput = 0;
loglevel_t loglevel = 0;


static const struct option lircmd_options[] = {
	{"help", no_argument, NULL, 'h'},
	{"version", no_argument, NULL, 'v'},
	{"nodaemon", no_argument, NULL, 'n'},
	{"options-file", required_argument, NULL, 'O'},
#       if defined(__linux__)
	{"uinput", no_argument, NULL, 'u'},
#       endif
	{0, 0, 0, 0}
};

static const char* const help = 
	"Usage: lircmd [options] [config-file]\n"
	"\t -h --help\t\tDisplay this message\n"
	"\t -v --version\t\tDisplay version\n"
	"\t -n --nodaemon\t\tDon't fork to background\n"
	"\t -O --options-file=file\tAlternative default options file\n"
#       if defined(__linux__)
	"\t -u --uinput\t\tGenerate Linux input events\n"
#       endif
;


int map_buttons(int b)
{
	switch (b) {
	case BUTTON1:
		return (MAP_BUTTON1);
	case BUTTON2:
		return (MAP_BUTTON2);
	default:
		return (MAP_BUTTON3);
	}
}

enum directive { move_n, move_ne, move_e, move_se,
	move_s, move_sw, move_w, move_nw,
	move_in, move_out,
	button1_down, button1_up, button1_toggle, button1_click,
	button2_down, button2_up, button2_toggle, button2_click,
	button3_down, button3_up, button3_toggle, button3_click,
	mouse_activate, mouse_toggle_activate, ignore
};

struct config_mouse {
	char *string;
	enum directive d;
	int x, y, z, down, up, toggle;
};

struct config_mouse config_table[] = {
	{"MOVE_N", move_n, 0, 1, 0, 0, 0, 0},
	{"MOVE_NE", move_ne, 1, 1, 0, 0, 0, 0},
	{"MOVE_E", move_e, 1, 0, 0, 0, 0, 0},
	{"MOVE_SE", move_se, 1, -1, 0, 0, 0, 0},
	{"MOVE_S", move_s, 0, -1, 0, 0, 0, 0},
	{"MOVE_SW", move_sw, -1, -1, 0, 0, 0, 0},
	{"MOVE_W", move_w, -1, 0, 0, 0, 0, 0},
	{"MOVE_NW", move_nw, -1, 1, 0, 0, 0, 0},
	{"MOVE_IN", move_in, 0, 0, -1, 0, 0, 0},
	{"MOVE_OUT", move_out, 0, 0, 1, 0, 0, 0},
	{"BUTTON1_DOWN", button1_down, 0, 0, 0, BUTTON1, 0, 0},
	{"BUTTON1_UP", button1_up, 0, 0, 0, 0, BUTTON1, 0},
	{"BUTTON1_TOGGLE", button1_toggle, 0, 0, 0, BUTTON1, BUTTON1, 1},
	{"BUTTON1_CLICK", button1_click, 0, 0, 0, BUTTON1, BUTTON1, 0},
	{"BUTTON2_DOWN", button2_down, 0, 0, 0, BUTTON2, 0, 0},
	{"BUTTON2_UP", button2_up, 0, 0, 0, 0, BUTTON2, 0},
	{"BUTTON2_TOGGLE", button2_toggle, 0, 0, 0, BUTTON2, BUTTON2, 1},
	{"BUTTON2_CLICK", button2_click, 0, 0, 0, BUTTON2, BUTTON2, 0},
	{"BUTTON3_DOWN", button3_down, 0, 0, 0, BUTTON3, 0, 0},
	{"BUTTON3_UP", button3_up, 0, 0, 0, 0, BUTTON3, 0},
	{"BUTTON3_TOGGLE", button3_toggle, 0, 0, 0, BUTTON3, BUTTON3, 1},
	{"BUTTON3_CLICK", button3_click, 0, 0, 0, BUTTON3, BUTTON3, 0},
	{"IGNORE", ignore, 0, 0, 0, 0, 0, 0},
	{NULL, ignore, 0, 0, 0, 0, 0, 0}
};

enum protocol { mouse_systems, imps_2, im_serial };

struct trans_mouse {
	struct trans_mouse *tm_next;
	char *tm_remote;
	char *tm_button;
	enum directive tm_directive;
} *tm_first = NULL;

enum state_button { button_up, button_down };
enum state_axis { axis_none, axis_up, axis_down };

struct state_mouse {
	int protocol;
	int always_active, toggle_active, active;
	int acc_start, acc_max, acc_fak;	/* defaults, acc_fak == acc_factor */
	enum state_button buttons[BUTTONS];
};

struct state_mouse new_ms, ms = {
	mouse_systems,
	1, 0, 0,
	2, 20, 2,
	{button_up, button_up, button_up}
};


const char *configfile = NULL;

int lircd = -1;
int lircm = -1;

sig_atomic_t hup = 0;

struct trans_mouse *read_config(FILE * fd);

void freetm(struct trans_mouse *tm_all)
{
	struct trans_mouse *tm;

	while (tm_all != NULL) {
		if (tm_all->tm_remote != ALL && tm_all->tm_remote != NULL)
			free(tm_all->tm_remote);
		if (tm_all->tm_button != ALL && tm_all->tm_button != NULL)
			free(tm_all->tm_button);
		tm = tm_all;
		tm_all = tm->tm_next;
		free(tm);
	}
}

void sigterm(int sig)
{
	/* not safe in a signal handler *//*freetm(tm_first); */

	shutdown(lircd, 2);
	close(lircd);

	if (lircm != -1) {
		shutdown(lircm, 2);
		close(lircm);
		lircm = -1;
	}
#if defined(__linux__)
	if (uinputfd != -1) {
		ioctl(uinputfd, UI_DEV_DESTROY);
		close(uinputfd);
		uinputfd = -1;
	}
#endif

	signal(sig, SIG_DFL);
	raise(sig);
}

void sighup(int sig)
{
	hup = 1;
}

void dohup(void)
{
	FILE *fd;
	struct trans_mouse *tm_list;

	fd = fopen(configfile, "r");
	if (fd == NULL) {
		syslog(LOG_WARNING, "could not open config file: %m");
		return;
	}
	tm_list = read_config(fd);
	fclose(fd);
	if (tm_list == (void *)-1) {
		syslog(LOG_WARNING, "reading of config file failed");
	} else {
		freetm(tm_first);
		tm_first = tm_list;
		ms = new_ms;
	}
}


void daemonize(void)
{
	if (daemon(0, 0) == -1) {
		fprintf(stderr, "%s: daemon() failed\n", progname);
		perror(progname);
		exit(EXIT_FAILURE);
	}
	umask(0);
}


int setup_uinputfd(const char *name)
{
#if defined(__linux__)
	int fd;
	struct uinput_user_dev dev;

	/* Open a uinput device. */
	fd = open("/dev/input/uinput", O_RDWR);
	if (fd == -1) {
		fd = open("/dev/uinput", O_RDWR);
		if (fd == -1) {
			fd = open("/dev/misc/uinput", O_RDWR);
			if (fd == -1) {
				fprintf(stderr, "could not open %s\n", "uinput");
				perror(NULL);
				return -1;
			}
		}
	}
	memset(&dev, 0, sizeof(dev));
	strncpy(dev.name, name, sizeof(dev.name));
	dev.name[sizeof(dev.name) - 1] = 0;
	if (write(fd, &dev, sizeof(dev)) != sizeof(dev)) {
		goto setup_error;
	}

	/* Configure support for the left, right and middle mouse buttons. */
	if ((ioctl(fd, UI_SET_EVBIT, EV_KEY) != 0) || (ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) != 0)
	    || (ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE) != 0) || (ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) != 0)) {
		goto setup_error;
	}

	/* Configure support for the relative mouse location and scroll wheel */
	if ((ioctl(fd, UI_SET_EVBIT, EV_REL) != 0) || (ioctl(fd, UI_SET_RELBIT, REL_X) != 0)
	    || (ioctl(fd, UI_SET_RELBIT, REL_Y) != 0) || (ioctl(fd, UI_SET_RELBIT, REL_WHEEL) != 0)) {
		goto setup_error;
	}

	/* Register the device with the input subsystem. */
	if (ioctl(fd, UI_DEV_CREATE) != 0) {
		goto setup_error;
	}
	return fd;

setup_error:
	fprintf(stderr, "could not setup %s\n", "uinput");
	perror(NULL);
	close(fd);
#endif
	return -1;
}

void write_uinput(__u16 type, __u16 code, __s32 value)
{
#ifdef __linux__
	struct input_event event;

	memset(&event, 0, sizeof(event));
	event.type = type;
	event.code = code;
	event.value = value;

	if (write(uinputfd, &event, sizeof(event)) != sizeof(event)) {
		static int once = 1;

		if (once) {
			int errno_save = errno;

			once = 0;
			syslog(LOG_ERR, "writing to uinput failed");
			errno = errno_save;
			syslog(LOG_ERR, "%m");
		}
	}
#endif
}

void msend(int dx, int dy, int dz, int rep, int buttp, int buttr)
{
	static int buttons = 0;
	int i;
	int f = 1;
	char buffer[5];

	if (rep >= ms.acc_start) {
		if (rep * ms.acc_fak >= ms.acc_max) {
			f = ms.acc_max;
		} else {
			f = rep * ms.acc_fak;
		}
	}

	buttons |= buttp;
	buttons &= ~buttr;

	switch (ms.protocol) {
	case mouse_systems:
		buffer[0] = ~(buttons | 0x78);

		buffer[1] = dx;
		buffer[2] = dy;
		buffer[3] = buffer[4] = 0;
		for (i = 0; i < f; i++) {
			chk_write(lircm, buffer, 5);
		}
		break;
	case imps_2:
		buffer[0] = ((buttons & BUTTON1) ? 0x01 : 0x00)
		    | ((buttons & BUTTON3) ? 0x02 : 0x00)
		    | ((buttons & BUTTON2) ? 0x04 : 0x00)
		    | 0x08 | (dx < 0 ? 0x10 : 0x00)
		    | (dy < 0 ? 0x20 : 0x00);
		buffer[1] = dx + (dx >= 0 ? 0 : 256);
		buffer[2] = dy + (dy >= 0 ? 0 : 256);
		buffer[3] = dz;
		for (i = 0; i < f; i++) {
			chk_write(lircm, buffer, 4);
		}
		break;
	case im_serial:
		dy = -dy;
		buffer[0] = ((buttons & BUTTON1) ? 0x20 : 0x00)
		    | ((buttons & BUTTON3) ? 0x10 : 0x00)
		    | 0x40 | ((dx & 0xC0) >> 6)
		    | ((dy & 0xC0) >> 4);
		buffer[1] = dx & ~0xC0;
		buffer[2] = dy & ~0xC0;
		buffer[3] = ((dz < 0) ? 0x0f : 0x00)
		    | ((dz > 0) ? 0x01 : 0x00)
		    | ((buttons & BUTTON2) ? 0x10 : 0x00);

		for (i = 0; i < f; i++) {
			chk_write(lircm, buffer, 4);
		}
		break;
	}

#if defined(__linux__)
	if (uinputfd != -1) {
		for (i = 0; i < f; i++) {
			if ((dx != 0) || (dy != 0)) {
				write_uinput(EV_REL, REL_X, dx);
				write_uinput(EV_REL, REL_Y, dy);
				write_uinput(EV_SYN, SYN_REPORT, 0);
			}
			if (dz != 0) {
				write_uinput(EV_REL, REL_WHEEL, dz);
				write_uinput(EV_SYN, SYN_REPORT, 0);
			}
			if (buttp & BUTTON1) {
				write_uinput(EV_KEY, BTN_LEFT, 1);
				write_uinput(EV_SYN, SYN_REPORT, 0);
			}
			if (buttr & BUTTON1) {
				write_uinput(EV_KEY, BTN_LEFT, 0);
				write_uinput(EV_SYN, SYN_REPORT, 0);
			}
			if (buttp & BUTTON2) {
				write_uinput(EV_KEY, BTN_MIDDLE, 1);
				write_uinput(EV_SYN, SYN_REPORT, 0);
			}
			if (buttr & BUTTON2) {
				write_uinput(EV_KEY, BTN_MIDDLE, 0);
				write_uinput(EV_SYN, SYN_REPORT, 0);
			}
			if (buttp & BUTTON3) {
				write_uinput(EV_KEY, BTN_RIGHT, 1);
				write_uinput(EV_SYN, SYN_REPORT, 0);
			}
			if (buttr & BUTTON3) {
				write_uinput(EV_KEY, BTN_RIGHT, 0);
				write_uinput(EV_SYN, SYN_REPORT, 0);
			}
		}
	}
#endif
}

void mouse_move(int dx, int dy, int dz, int rep)
{
	msend(dx, dy, dz, rep, 0, 0);
}

void mouse_button(int down, int up, int rep)
{
	if (rep == 0) {
		msend(0, 0, 0, rep, down, up);
		if (down & BUTTON1)
			ms.buttons[map_buttons(BUTTON1)] = button_down;
		if (down & BUTTON2)
			ms.buttons[map_buttons(BUTTON2)] = button_down;
		if (down & BUTTON3)
			ms.buttons[map_buttons(BUTTON3)] = button_down;
		if (up & BUTTON1)
			ms.buttons[map_buttons(BUTTON1)] = button_up;
		if (up & BUTTON2)
			ms.buttons[map_buttons(BUTTON2)] = button_up;
		if (up & BUTTON3)
			ms.buttons[map_buttons(BUTTON3)] = button_up;
	}
}

/*
  You don't understand this funktion?
  Never mind, I don't understand it, too.
*/

void mouse_circle(int r, int dirx, int diry)
{
	int i, d, incX, incY, x, y;
	int dd[8] = {
		1, 0, -1, -1, -1, 0, 1, 1
	};

	for (i = 0; i < 8; i++) {
		d = 1 - r;
		incX = 0;
		incY = 2 * r;
		x = 0;
		y = r;
		while (x < y) {
			if (d >= 0) {
				y--;
				incY -= 2;
				d -= incY;
				mouse_move(dirx * dd[i], diry * dd[(i + 8 - 6) % 8], 0, 0);
			} else {
				mouse_move(dirx * dd[(i + 8 - 1) % 8], diry * dd[(i + 8 - 7) % 8], 0, 0);
			}
			x++;
			incX += 2;
			d += incX + 1;
			usleep(1);
		}
	}
}

void activate()
{
	ms.active = 1;
	mouse_circle(CIRCLE, 1, 1);
}

void deactivate()
{
	/* all buttons up */
	mouse_button(0, BUTTON1 | BUTTON2 | BUTTON3, 0);
	ms.active = 0;
	mouse_circle(CIRCLE, -1, 1);
}

void mouse_conv(int rep, char *button, char *remote)
{
	struct trans_mouse *tm;
	int found = 0;

	tm = tm_first;
	while (tm != NULL) {
		if (tm->tm_remote != ALL) {
			if (strcasecmp(remote, tm->tm_remote) != 0) {
				tm = tm->tm_next;
				continue;
			}
		}
		if (tm->tm_button != ALL) {
			if (strcasecmp(button, tm->tm_button) != 0) {
				tm = tm->tm_next;
				continue;
			}
		}
		if (tm->tm_directive == mouse_activate) {
			if (ms.active == 0 && ms.always_active == 0) {
				activate();
			}
		} else if (tm->tm_directive == mouse_toggle_activate && rep == 0) {
			if (ms.always_active == 0) {
				if (ms.active == 0) {
					activate();
					ms.toggle_active = 1;
				} else {
					deactivate();
				}
			}
		}

		if (tm->tm_directive != ignore && (ms.active || ms.always_active)) {
			int i;
			for (i = 0; config_table[i].string != NULL; i++) {
				if (tm->tm_directive == config_table[i].d) {
					int x, y, z, up, down, toggle;

					x = config_table[i].x;
					y = config_table[i].y;
					z = config_table[i].z;
					down = config_table[i].down;
					up = config_table[i].up;
					toggle = config_table[i].toggle;

					if (x || y || z) {
						mouse_move(x, y, z, rep);
					}
					if (toggle) {
						/*
						   assert(down==up);
						   assert(up==BUTTON1
						   || up==BUTTON2
						   || up==BUTTON3);
						 */
						if (ms.buttons[map_buttons(up)] == button_up)
							mouse_button(down, 0, rep);
						else
							mouse_button(0, up, rep);
					} else {
						if (down && up) {	/* click */
							mouse_button(down, 0, rep);
#ifdef CLICK_DELAY
							usleep(CLICK_DELAY);
#endif
							mouse_button(0, up, rep);
						} else if (down || up) {
							mouse_button(down, up, rep);
						}
					}
					break;
				}
			}

		}
		found = 1;
		tm = tm->tm_next;
	}
	if (found == 0) {
		if (ms.active == 1 && ms.always_active == 0 && ms.toggle_active == 0) {
			deactivate();
		}
	}
}

struct trans_mouse *read_config(FILE * fd)
{
	char buffer[PACKET_SIZE];
	char *directives, *remote, *button;
	enum directive d;
	int len;
	int line;
	struct trans_mouse *tm_new, *tm_list, *tm_last = NULL;

	tm_list = NULL;
	new_ms = ms;
	new_ms.always_active = 1;
	new_ms.toggle_active = 0;
	line = 0;
	while (fgets(buffer, PACKET_SIZE, fd) != NULL) {
		line++;
		len = strlen(buffer);
		if (len == PACKET_SIZE - 1 && buffer[len - 1] != '\n') {
			syslog(LOG_ERR, "line %d too long in config file", line);
			freetm(tm_list);
			return ((void *)-1);
		}
		if (len > 0) {
			len--;
			if (buffer[len] == '\n')
				buffer[len] = 0;
		}
		if (len > 0) {
			len--;
			if (buffer[len] == '\r')
				buffer[len] = 0;
		}

		/* ignore comments */
		if (buffer[0] == '#')
			continue;

		directives = strtok(buffer, WHITE_SPACE);
		/* ignore empty lines */
		if (directives == NULL)
			continue;

		if (strcasecmp("PROTOCOL", directives) == 0) {
			char *name;

			name = strtok(NULL, WHITE_SPACE);
			if (name != NULL) {
				if (strcasecmp("MouseSystems", name) == 0) {
					new_ms.protocol = mouse_systems;
				} else if (strcasecmp("IMPS/2", name) == 0) {
					new_ms.protocol = imps_2;
				} else if (strcasecmp("IntelliMouse", name) == 0) {
					new_ms.protocol = im_serial;
				} else {
					syslog(LOG_WARNING, "unknown protocol %s", name);
				}
			}
			if (name == NULL || strtok(NULL, WHITE_SPACE) != NULL) {
				syslog(LOG_WARNING, "invalid line %d in config file ignored", line);
				continue;
			}
			continue;
		}

		if (strcasecmp("ACCELERATOR", directives) == 0) {
			char *number;

			number = strtok(NULL, WHITE_SPACE);
			if (number != NULL)
				new_ms.acc_start = atoi(number);
			number = strtok(NULL, WHITE_SPACE);
			if (number != NULL)
				new_ms.acc_max = atoi(number);
			number = strtok(NULL, WHITE_SPACE);
			if (number != NULL)
				new_ms.acc_fak = atoi(number);
			if (strtok(NULL, WHITE_SPACE) != NULL) {
				syslog(LOG_WARNING, "invalid line %d in config file ignored", line);
				new_ms.acc_start = ms.acc_start;
				new_ms.acc_max = ms.acc_max;
				new_ms.acc_fak = ms.acc_fak;
				continue;
			}
			continue;
		}

		remote = strtok(NULL, WHITE_SPACE);
		button = strtok(NULL, WHITE_SPACE);
		if (remote == NULL || button == NULL || strtok(NULL, WHITE_SPACE) != NULL) {
			syslog(LOG_WARNING, "invalid line %d in config file ignored", line);
			continue;
		}

		if (strcasecmp("ACTIVATE", directives) == 0) {
			d = mouse_activate;
			new_ms.always_active = 0;
		} else if (strcasecmp("TOGGLE_ACTIVATE", directives) == 0) {
			d = mouse_toggle_activate;
			new_ms.always_active = 0;
		} else {
			int i;

			d = mouse_activate;	/* make compiler happy */
			for (i = 0; config_table[i].string != NULL; i++) {
				if (strcasecmp(config_table[i].string, directives) == 0) {
					d = config_table[i].d;
					break;
				}
			}
			if (config_table[i].string == NULL) {
				syslog(LOG_WARNING, "unknown directive \"%s\" ignored", directives);
				continue;
			}
		}

		if (strcmp("*", remote) == 0)
			remote = ALL;
		else
			remote = strdup(remote);
		if (strcmp("*", button) == 0)
			button = ALL;
		else
			button = strdup(button);

		tm_new = malloc(sizeof(struct trans_mouse));
		if (remote == NULL || button == NULL || tm_new == NULL) {
			syslog(LOG_ERR, "out of memory");
			if (remote != NULL)
				free(remote);
			if (button != NULL)
				free(button);
			if (tm_new != NULL)
				free(tm_new);
			free(tm_list);
			return ((void *)-1);
		}
		tm_new->tm_next = NULL;
		tm_new->tm_remote = remote;
		tm_new->tm_button = button;
		tm_new->tm_directive = d;
		if (tm_list == NULL) {
			tm_list = tm_new;
			tm_last = tm_new;
		} else {
			tm_last->tm_next = tm_new;
			tm_last = tm_new;
		}
	}
	return (tm_list);
}



static void lircmd_add_defaults(void)
{
	char level[4];
	snprintf(level, sizeof(level), "%d", lirc_log_defaultlevel());

	const char* const defaults[] = {
		"lircmd:nodaemon", 	"False",
		"lircmd:uinput", 	"False",
		"lircmd:configfile",    LIRCMDCFGFILE,
		"lircmd:debug",		level,
		(const char*)NULL, 	(const char*)NULL
	};
	options_add_defaults(defaults);
}


static void lircmd_parse_options(int argc,  char** const argv)
{
	int c;

#       if defined(__linux__)
	const char* const optstring =  "hvnuO";
#       else
	const char* const optstring = "hvnO";
#       endif

	lircmd_add_defaults();
	optind = 1;
	while ((c = getopt_long(argc, argv, optstring, lircmd_options, NULL))
		!= -1 )
	{
		switch (c) {
		case 'h':
			printf(help);
			exit(EXIT_SUCCESS);
		case 'v':
			printf("lircmd %s\n",  VERSION);
			exit(EXIT_SUCCESS);
		case 'O':
			break;
		case 'n':
			options_set_opt("lircd:nodaemon", "True");
			break;
#               if defined(__linux__)
		case 'u':
			options_set_opt("lircd:uinput", "True");
			break;
#               endif
		default:
			fprintf(stderr,
				"Usage: lircmd [options] [config-file]\n");
			exit(EXIT_FAILURE);
		}
	}
	if (optind == argc - 1) {
		options_set_opt("configfile", argv[optind]);
	} else if (optind != argc) {
		fprintf(stderr, "lircmd: invalid argument count\n");
		exit(EXIT_FAILURE);
	}
}


void loop()
{
	ssize_t len = 0;
	char buffer[PACKET_SIZE + 1];
	int rep, ret;
	char button[PACKET_SIZE + 1];
	char remote[PACKET_SIZE + 1];
	char *end;
	int end_len = 0;
	sigset_t block;

	sigemptyset(&block);
	sigaddset(&block, SIGHUP);
	buffer[0] = 0;
	while (1) {
		if (hup) {
			dohup();
			hup = 0;
		}
		if (strchr(buffer, '\n') == NULL) {

			sigprocmask(SIG_UNBLOCK, &block, NULL);
			len = read(lircd, buffer + end_len, PACKET_SIZE - end_len);
			sigprocmask(SIG_BLOCK, &block, NULL);
			if (len <= 0) {
				if (len == -1 && errno == EINTR)
					continue;
				raise(SIGTERM);
			}
		}
		buffer[len + end_len] = 0;
		ret = sscanf(buffer, "%*x %x %s %s\n", &rep, button, remote);
		end = strchr(buffer, '\n');
		if (end == NULL) {
			end_len = 0;
			continue;
		}
		end++;
		end_len = strlen(end);
		memmove(buffer, end, end_len + 1);
		if (ret == 3) {
			mouse_conv(rep, button, remote);
		}
	}

}

int main(int argc, char **argv)
{
	FILE *fd;
	struct sigaction act;
	struct sockaddr_un addr;
	sigset_t block;
	int nodaemon = 0;
	const char *filename;
	loglevel_t loglevel;
	const char* level;

	options_load(argc, argv, NULL, lircmd_parse_options);
	useuinput = options_getboolean("lircmd:uinput");
	nodaemon = options_getboolean("lircmd:nodaemon");
	configfile = options_getstring("lircmd:configfile");
	level = options_getstring("lircmd:debug");
	loglevel = string2loglevel(level);
	lirc_log_open("lircmd",
		       nodaemon,
		       loglevel == LIRC_BADLEVEL? LIRC_DEBUG : loglevel);

	/* connect to lircd */
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, LIRCD);
	lircd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lircd == -1) {
		fprintf(stderr, "%s: could not open socket\n", progname);
		perror(progname);
		exit(EXIT_FAILURE);
	};
	if (connect(lircd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		fprintf(stderr, "%s: could not connect to socket: %s\n",
			progname, addr.sun_path);
		perror(progname);
		exit(EXIT_FAILURE);
	};

	/* either create uinput device or fifo device */
	uinputfd = -1;
	lircm = -1;
	if (useuinput) {
		/* create uinput device */

		uinputfd = setup_uinputfd(progname);
	} else {
		/* open fifo */

		if (mkfifo(LIRCM, 0644) == -1) {
			if (errno != EEXIST) {
				fprintf(stderr, "%s: could not create fifo\n", progname);
				perror(progname);
				exit(EXIT_FAILURE);
			}
		}

		lircm = open(LIRCM, O_RDWR | O_NONBLOCK);
		if (lircm == -1) {
			fprintf(stderr, "%s: could not open fifo\n", progname);
			perror(progname);
			exit(EXIT_FAILURE);
		}
	}

	/* read config file */

	filename = configfile;
	if (configfile == NULL)
		filename = LIRCMDCFGFILE;
	fd = fopen(filename, "r");
	if (fd == NULL && errno == ENOENT && configfile == NULL) {
		int save_errno = errno;
		filename = LIRCMDOLDCFGFILE;
		fd = fopen(filename, "r");
		errno = save_errno;
	}
	if (fd == NULL) {
		fprintf(stderr, "%s: could not open config file\n", progname);
		perror(progname);
		exit(EXIT_FAILURE);
	}
	configfile = filename;
	tm_first = read_config(fd);
	fclose(fd);
	if (tm_first == (void *)-1) {
		fprintf(stderr, "%s: reading of config file failed\n", progname);
		exit(EXIT_FAILURE);
	} else {
		ms = new_ms;
	}

	if (!nodaemon)
		daemonize();
	signal(SIGPIPE, SIG_IGN);

	act.sa_handler = sigterm;
	sigfillset(&act.sa_mask);
	act.sa_flags = SA_RESTART;	/* don't fiddle with EINTR */
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);

	/* block SIGHUP first */
	sigemptyset(&block);
	sigaddset(&block, SIGHUP);
	sigprocmask(SIG_BLOCK, &block, NULL);

	act.sa_handler = sighup;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;	/* need EINTR in loop() */
	sigaction(SIGHUP, &act, NULL);

	loop();

	/* never reached */
	return (EXIT_SUCCESS);
}
