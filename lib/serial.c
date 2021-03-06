
/****************************************************************************
 ** serial.c ****************************************************************
 ****************************************************************************
 *
 * common routines for hardware that uses the standard serial port driver
 *
 * Copyright (C) 1999 Christoph Bartelmus <lirc@bartelmus.de>
 *
 */

/**
 * @file serial.c
 * @author Christoph Bartelmus
 * @brief Implements serial.h
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef LIRC_LOCKDIR
#define LIRC_LOCKDIR "/var/lock/lockdev"
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#if defined (__linux__)
#include <linux/serial.h>	/* for 'struct serial_struct' to set custom
				   baudrates */
#endif

#include "lirc/lirc_log.h"

int tty_reset(int fd)
{
	struct termios options;

	if (tcgetattr(fd, &options) == -1) {
		LOGPRINTF(1, "tty_reset(): tcgetattr() failed");
		LOGPERROR(1, "tty_reset()");
		return (0);
	}
	cfmakeraw(&options);
	if (tcsetattr(fd, TCSAFLUSH, &options) == -1) {
		LOGPRINTF(1, "tty_reset(): tcsetattr() failed");
		LOGPERROR(1, "tty_reset()");
		return (0);
	}
	return (1);
}

int tty_setrtscts(int fd, int enable)
{
	struct termios options;

	if (tcgetattr(fd, &options) == -1) {
		LOGPRINTF(1, "%s: tcgetattr() failed", __FUNCTION__);
		LOGPERROR(1, __FUNCTION__);
		return (0);
	}
	if (enable) {
		options.c_cflag |= CRTSCTS;
	} else {
		options.c_cflag &= ~CRTSCTS;
	}
	if (tcsetattr(fd, TCSAFLUSH, &options) == -1) {
		LOGPRINTF(1, "%s: tcsetattr() failed", __FUNCTION__);
		LOGPERROR(1, __FUNCTION__);
		return (0);
	}
	return (1);
}

int tty_setdtr(int fd, int enable)
{
	int cmd, sts;

	if (ioctl(fd, TIOCMGET, &sts) < 0) {
		LOGPRINTF(1, "%s: ioctl(TIOCMGET) failed", __FUNCTION__);
		LOGPERROR(1, __FUNCTION__);
		return (0);
	}
	if (((sts & TIOCM_DTR) == 0) && enable) {
		LOGPRINTF(1, "%s: 0->1", __FUNCTION__);
	} else if ((!enable) && (sts & TIOCM_DTR)) {
		LOGPRINTF(1, "%s: 1->0", __FUNCTION__);
	}
	if (enable) {
		cmd = TIOCMBIS;
	} else {
		cmd = TIOCMBIC;
	}
	sts = TIOCM_DTR;
	if (ioctl(fd, cmd, &sts) < 0) {
		LOGPRINTF(1, "%s: ioctl(TIOCMBI(S|C)) failed", __FUNCTION__);
		LOGPERROR(1, __FUNCTION__);
		return (0);
	}
	return (1);
}

int tty_setbaud(int fd, int baud)
{
	struct termios options;
	int speed;
#if defined (__linux__)
	int use_custom_divisor = 0;
	struct serial_struct serinfo;
#endif

	switch (baud) {
	case 300:
		speed = B300;
		break;
	case 1200:
		speed = B1200;
		break;
	case 2400:
		speed = B2400;
		break;
	case 4800:
		speed = B4800;
		break;
	case 9600:
		speed = B9600;
		break;
	case 19200:
		speed = B19200;
		break;
	case 38400:
		speed = B38400;
		break;
	case 57600:
		speed = B57600;
		break;
	case 115200:
		speed = B115200;
		break;
#ifdef B230400
	case 230400:
		speed = B230400;
		break;
#endif
#ifdef B460800
	case 460800:
		speed = B460800;
		break;
#endif
#ifdef B500000
	case 500000:
		speed = B500000;
		break;
#endif
#ifdef B576000
	case 576000:
		speed = B576000;
		break;
#endif
#ifdef B921600
	case 921600:
		speed = B921600;
		break;
#endif
#ifdef B1000000
	case 1000000:
		speed = B1000000;
		break;
#endif
#ifdef B1152000
	case 1152000:
		speed = B1152000;
		break;
#endif
#ifdef B1500000
	case 1500000:
		speed = B1500000;
		break;
#endif
#ifdef B2000000
	case 2000000:
		speed = B2000000;
		break;
#endif
#ifdef B2500000
	case 2500000:
		speed = B2500000;
		break;
#endif
#ifdef B3000000
	case 3000000:
		speed = B3000000;
		break;
#endif
#ifdef B3500000
	case 3500000:
		speed = B3500000;
		break;
#endif
#ifdef B4000000
	case 4000000:
		speed = B4000000;
		break;
#endif
	default:
#if defined (__linux__)
		speed = B38400;
		use_custom_divisor = 1;
		break;
#else
		LOGPRINTF(1, "tty_setbaud(): bad baud rate %d", baud);
		return (0);
#endif
	}
	if (tcgetattr(fd, &options) == -1) {
		LOGPRINTF(1, "tty_setbaud(): tcgetattr() failed");
		LOGPERROR(1, "tty_setbaud()");
		return (0);
	}
	(void)cfsetispeed(&options, speed);
	(void)cfsetospeed(&options, speed);
	if (tcsetattr(fd, TCSAFLUSH, &options) == -1) {
		LOGPRINTF(1, "tty_setbaud(): tcsetattr() failed");
		LOGPERROR(1, "tty_setbaud()");
		return (0);
	}
#if defined (__linux__)
	if (use_custom_divisor) {
		if (ioctl(fd, TIOCGSERIAL, &serinfo) < 0) {
			LOGPRINTF(1, "tty_setbaud(): TIOCGSERIAL failed");
			LOGPERROR(1, "tty_setbaud()");
			return (0);
		}
		serinfo.flags &= ~ASYNC_SPD_MASK;
		serinfo.flags |= ASYNC_SPD_CUST;
		serinfo.custom_divisor = serinfo.baud_base / baud;
		if (ioctl(fd, TIOCSSERIAL, &serinfo) < 0) {
			LOGPRINTF(1, "tty_setbaud(): TIOCSSERIAL failed");
			LOGPERROR(1, "tty_setbaud()");
			return (0);
		}
	}
#endif
	return (1);
}

int tty_setcsize(int fd, int csize)
{
	struct termios options;
	int size;

	switch (csize) {
	case 5:
		size = CS5;
		break;
	case 6:
		size = CS6;
		break;
	case 7:
		size = CS7;
		break;
	case 8:
		size = CS8;
		break;
	default:
		LOGPRINTF(1, "tty_setcsize(): bad csize rate %d", csize);
		return (0);
	}
	if (tcgetattr(fd, &options) == -1) {
		LOGPRINTF(1, "tty_setcsize(): tcgetattr() failed");
		LOGPERROR(1, "tty_setcsize()");
		return (0);
	}
	options.c_cflag &= ~CSIZE;
	options.c_cflag |= size;
	if (tcsetattr(fd, TCSAFLUSH, &options) == -1) {
		LOGPRINTF(1, "tty_setcsize(): tcsetattr() failed");
		LOGPERROR(1, "tty_setcsize()");
		return (0);
	}
	return (1);
}

/**
 * Creates a lock file of the type /var/local/LCK.. + name
 * @param name Name of the device
 * @return non-zero if successful
 * @see  http://www.pathname.com/fhs/2.2/fhs-5.9.html
 */
int tty_create_lock(const char *name)
{
	char filename[FILENAME_MAX + 1];
	char symlink[FILENAME_MAX + 1];
	char cwd[FILENAME_MAX + 1];
	const char *last, *s;
	char id[10 + 1 + 1];
	int lock;
	int len;

	strcpy(filename, LIRC_LOCKDIR "/LCK..");

	last = strrchr(name, '/');
	if (last != NULL)
		s = last + 1;
	else
		s = name;

	if (strlen(filename) + strlen(s) > FILENAME_MAX) {
		logprintf(LIRC_ERROR, "invalid filename \"%s%s\"", filename, s);
		return (0);
	}
	strcat(filename, s);

tty_create_lock_retry:
	if ((len = snprintf(id, 10 + 1 + 1, "%10d\n", getpid())) == -1) {
		logprintf(LIRC_ERROR, "invalid pid \"%d\"", getpid());
		return (0);
	}
	lock = open(filename, O_CREAT | O_EXCL | O_WRONLY, 0644);
	if (lock == -1) {
		logperror(LIRC_ERROR, "could not create lock file \"%s\"", filename);
		lock = open(filename, O_RDONLY);
		if (lock != -1) {
			pid_t otherpid;

			id[10 + 1] = 0;
			if (read(lock, id, 10 + 1) == 10 + 1 && read(lock, id, 1) == 0
			    && sscanf(id, "%d\n", &otherpid) > 0) {
				if (kill(otherpid, 0) == -1 && errno == ESRCH) {
					logprintf(LIRC_WARNING, "detected stale lockfile %s", filename);
					close(lock);
					if (unlink(filename) != -1) {
						logprintf(LIRC_WARNING, "stale lockfile removed");
						goto tty_create_lock_retry;
					} else {
						logperror(LIRC_ERROR,
                                                          "could not remove stale lockfile");
					}
					return (0);
				} else {
					logprintf(LIRC_ERROR, "%s is locked by PID %d", name, otherpid);
				}
			} else {
				logprintf(LIRC_ERROR, "invalid lockfile %s encountered", filename);
			}
			close(lock);
		}
		return (0);
	}
	if (write(lock, id, len) != len) {
		logperror(LIRC_ERROR, "could not write pid to lock file");
		close(lock);
		if (unlink(filename) == -1) {
			logperror(LIRC_ERROR, "could not delete file \"%s\"", filename);
			/* FALLTHROUGH */
		}
		return (0);
	}
	if (close(lock) == -1) {
		logperror(LIRC_ERROR, "could not close lock file");
		if (unlink(filename) == -1) {
			logperror(LIRC_ERROR, "could not delete file \"%s\"", filename);
			/* FALLTHROUGH */
		}
		return (0);
	}

	if ((len = readlink(name, symlink, FILENAME_MAX)) == -1) {
		if (errno != EINVAL) {	/* symlink */
			logperror(LIRC_ERROR, "readlink() failed for \"%s\"", name);
			if (unlink(filename) == -1) {
				logperror(LIRC_ERROR,
                                          "could not delete file \"%s\"", filename);
				/* FALLTHROUGH */
			}
			return (0);
		}
	} else {
		symlink[len] = 0;

		if (last) {
			char dirname[FILENAME_MAX + 1];

			if (getcwd(cwd, FILENAME_MAX) == NULL) {
				logperror(LIRC_ERROR, "getcwd() failed");
				if (unlink(filename) == -1) {
					logperror(LIRC_ERROR,
                                                  "could not delete file \"%s\"",
                                                  filename);
					/* FALLTHROUGH */
				}
				return (0);
			}

			strcpy(dirname, name);
			dirname[strlen(name) - strlen(last)] = 0;
			if (chdir(dirname) == -1) {
				logperror(LIRC_ERROR,
                                          "chdir() to \"%s\" failed", dirname);
				if (unlink(filename) == -1) {
					logperror(LIRC_ERROR,
                                                  "could not delete file \"%s\"",
                                                  filename);
					/* FALLTHROUGH */
				}
				return (0);
			}
		}
		if (tty_create_lock(symlink) == -1) {
			if (unlink(filename) == -1) {
				logperror(LIRC_ERROR,
                                          "could not delete file \"%s\"", filename);
				/* FALLTHROUGH */
			}
			return (0);
		}
		if (last) {
			if (chdir(cwd) == -1) {
				logperror(LIRC_ERROR, "chdir() to \"%s\" failed", cwd);
				if (unlink(filename) == -1) {
					logperror(LIRC_ERROR,
                                                  "could not delete file \"%s\"",
                                                  filename);
					/* FALLTHROUGH */
				}
				return (0);
			}
		}
	}
	return (1);
}

/**
 * Delete any lock(s) owned by this process.
 * @return 0 on errors, else 1.
 * @see  http://www.pathname.com/fhs/2.2/fhs-5.9.html
 */
int tty_delete_lock(void)
{
	DIR *dp;
	struct dirent *ep;
	int lock;
	int len;
	char id[20] = {'\0'};
	char filename[FILENAME_MAX + 1];
	long pid;
	int retval = 1;

	dp = opendir(LIRC_LOCKDIR);
	if (dp != NULL) {
		while ((ep = readdir(dp))) {
			if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0) {
				retval = 0;
				continue;
			}
			strcpy(filename, LIRC_LOCKDIR "/");
			if (strlen(filename) + strlen(ep->d_name) > FILENAME_MAX) {
				retval = 0;
				continue;
			}
			strcat(filename, ep->d_name);
			if (strstr(filename, "LCK..") == NULL) {
				logprintf(LIRC_DEBUG,
					  "Ignoring non-LCK.. logfile %s",
					  filename);
				retval = 0;
				continue;
			}
			lock = open(filename, O_RDONLY);
			if (lock == -1) {
				retval = 0;
				continue;
			}
			len = read(lock, id, sizeof(id) - 1);
			close(lock);
			if (len <= 0) {
				retval = 0;
				continue;
			}
			pid = strtol(id, NULL, 10);
			if (pid == LONG_MIN || pid == LONG_MAX || pid == 0) {
				logprintf(LIRC_DEBUG,
					  "Can't parse lockfile %s (ignored)",
					  filename);
				retval = 0;
				continue;
			}
			if (pid == getpid()) {
				if (unlink(filename) == -1) {
					logperror(LIRC_ERROR,
                                                  "could not delete file \"%s\"",
                                                  filename);
					retval = 0;
					continue;
				}
			}
		}
		closedir(dp);
	} else {
		logprintf(LIRC_ERROR, "could not open directory \"" LIRC_LOCKDIR "\"");
		return (0);
	}
	return (retval);
}

int tty_set(int fd, int rts, int dtr)
{
	int mask;

	mask = rts ? TIOCM_RTS : 0;
	mask |= dtr ? TIOCM_DTR : 0;
	if (ioctl(fd, TIOCMBIS, &mask) == -1) {
		LOGPRINTF(1, "tty_set(): ioctl() failed");
		LOGPERROR(1, "tty_set()");
		return (0);
	}
	return (1);
}

int tty_clear(int fd, int rts, int dtr)
{
	int mask;

	mask = rts ? TIOCM_RTS : 0;
	mask |= dtr ? TIOCM_DTR : 0;
	if (ioctl(fd, TIOCMBIC, &mask) == -1) {
		LOGPRINTF(1, "tty_clear(): ioctl() failed");
		LOGPERROR(1, "tty_clear()");
		return (0);
	}
	return (1);
}

int tty_write(int fd, char byte)
{
	if (write(fd, &byte, 1) != 1) {
		LOGPRINTF(1, "tty_write(): write() failed");
		LOGPERROR(1, "tty_write()");
		return (-1);
	}
	/* wait until the stop bit of Control Byte is sent
	   (for 9600 baud rate, it takes about 100 msec */
	usleep(100 * 1000);

	/* we don't wait because tcdrain() does this for us */
	/* tcdrain(fd); */
	/* FIXME! but unfortunately this does not seem to be
	   implemented in 2.0.x kernels ... */
	return (1);
}

int tty_read(int fd, char *byte)
{
	fd_set fds;
	int ret;
	struct timeval tv;

	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	tv.tv_sec = 1;		/* timeout after 1 sec */
	tv.tv_usec = 0;
	ret = select(fd + 1, &fds, NULL, NULL, &tv);
	if (ret == 0) {
		logprintf(LIRC_ERROR, "tty_read(): timeout");
		return (-1);	/* received nothing, bad */
	} else if (ret != 1) {
		LOGPRINTF(1, "tty_read(): select() failed");
		LOGPERROR(1, "tty_read()");
		return (-1);
	}
	if (read(fd, byte, 1) != 1) {
		LOGPRINTF(1, "tty_read(): read() failed");
		LOGPERROR(1, "tty_read()");
		return (-1);
	}
	return (1);
}

int tty_write_echo(int fd, char byte)
{
	char reply;

	if (tty_write(fd, byte) == -1)
		return (-1);
	if (tty_read(fd, &reply) == -1)
		return (-1);
	LOGPRINTF(1, "sent: A%u D%01x reply: A%u D%01x", (((unsigned int)(unsigned char)byte) & 0xf0) >> 4,
		  ((unsigned int)(unsigned char)byte) & 0x0f, (((unsigned int)(unsigned char)reply) & 0xf0) >> 4,
		  ((unsigned int)(unsigned char)reply) & 0x0f);
	if (byte != reply) {
		logprintf(LIRC_ERROR, "Command mismatch.");
	}
	return (1);
}
