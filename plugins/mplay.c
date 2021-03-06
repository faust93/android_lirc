/****************************************************************************
 ** hw_mplay.c **************************************************************
 ****************************************************************************
 *
 * LIRC driver for Vlsys mplay usb ftdi serial port remote control.
 *
 * Driver inspire from hw_accent et hw_alsa_usb.
 *
 * The vlsys mplay is a remote control with an Ir receiver connected to the
 * usb bus via a ftdi driver. The device communicate with the host at 38400
 * 8N1.
 *
 * For each keypress on the remote controle, one code byte is transmitted
 * follow by regular fixe code byte for repetition if the key is held-down.
 * For example, if you press key 1, the remote first send 0x4d (key code)
 * and next regulary send 0x7e (repetetion code) as you held-down the key.
 * For key 2 you get 0x4e 0x7e 0x7e ...
 *
 * Copyright (c) 2007 Benoit Laurent <ben905@free.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */


#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#ifndef LIRC_IRTTY
#define LIRC_IRTTY "/dev/ttyUSB0"
#endif

#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>
#include <termios.h>

#include "lirc_driver.h"
#include "lirc/serial.h"

/* The mplay code length in bit */
#define MPLAY_CODE_LENGTH 8

/* Code value send by the mplay to indicate a repeatition of the last code */
#define MPLAY_REPEAT_CODE 0x7e

/* Mplay serial baud rate */
#define MPLAY_BAUD_RATE 38400

/* Max time in micro seconde between the reception of repetition code. After
   this time, we ignore the key repeat */
#define MAX_TIME_BETWEEN_TWO_REPETITION_CODE 500000

//Forwards:
extern int mplay_decode(struct ir_remote *remote, struct decode_ctx_t* ctx);

extern int mplay_init(void);
extern int mplay2_init(void);
extern int mplay_deinit(void);
extern char *mplay_rec(struct ir_remote *remotes);


/**************************************************************************
 * Definition of local struct that permit to save data from call to call
 * of the driver.
 **************************************************************************/
static struct {
	/* the last receive code */
	ir_code rc_code;
	/* Int wich indicate that the last reception was a repetition */
	int repeat_flag;
	/* Date of the last reception */
	struct timeval last_reception_time;
	/* Flag wich indicate a timeout between the reception of repetition
	   Some time the receiver lost a key code and only received
	   the associated repetition code. Then the driver interpret
	   this repetition as a repetition of the last receive key code
	   and not the lost one (ex: you press Volume+ after Volume-
	   and the sound continu to go down). To avoid this problem
	   we set a max time between two repetition. */
	int timeout_repetition_flag;
} mplay_local_data = {
	0, 0, {
0, 0}, 0};

/**************************************************************************
 * Definition of the standard internal hardware interface
 * use by lirc for the mplay device
 **************************************************************************/
const struct driver hw_mplay = {
	.name		=	"mplay",
	.device		=	LIRC_IRTTY,
	.features	=	LIRC_CAN_REC_LIRCCODE,
	.send_mode	=	0,
	.rec_mode	=	LIRC_MODE_LIRCCODE,
	.code_length	=	MPLAY_CODE_LENGTH,
	.init_func	=	mplay_init,
	.deinit_func	=	mplay_deinit,
	.open_func	=	default_open,
	.close_func	=	default_close,
	.send_func	=	NULL,
	.rec_func	=	mplay_rec,
	.decode_func	=	mplay_decode,
	.drvctl_func	=	NULL,
	.readdata	=	NULL,
	.api_version	=	2,
	.driver_version = 	"0.9.2",
	.info		=	"No info available"
};

/**************************************************************************
 * Definition of the standard internal hardware interface
 * use by lirc for the mplay v2 (Monueal Moncaso) devices
 **************************************************************************/
const struct driver hw_mplay2 = {
	.name		=	"mplay2",
	.device		=	LIRC_IRTTY,
	.features	=	LIRC_CAN_REC_LIRCCODE,
	.send_mode	=	0,
	.rec_mode	=	LIRC_MODE_LIRCCODE,
	.code_length	=	MPLAY_CODE_LENGTH,
	.init_func	=	mplay2_init,
	.deinit_func	=	mplay_deinit,
	.open_func	=	default_open,
	.close_func	=	default_close,
	.send_func	=	NULL,
	.rec_func	=	mplay_rec,
	.decode_func	=	mplay_decode,
	.drvctl_func	=	NULL,
	.readdata	=	NULL,
	.api_version	=	2,
	.driver_version = 	"0.9.2",
	.info		=	"No info available"
};
const struct driver* hardwares[] = { &hw_mplay, &hw_mplay2, NULL };


/**************************************************************************
 * Lock and initialize the serial port.
 * This function is called by the LIRC daemon when the first client
 * registers itself.
 * Return 1 on success, 0 on error.
 **************************************************************************/
int mplay_init(void)
{
	int result = 1;
	LOGPRINTF(1, "Entering mplay_init()");
	/* Creation of a lock file for the port */
	if (!tty_create_lock(drv.device)) {
		logprintf(LIRC_ERROR, "Could not create the lock file");
		LOGPRINTF(1, "Could not create the lock file");
		result = 0;
	}
	/* Try to open serial port */
	else if ((drv.fd = open(drv.device, O_RDWR | O_NONBLOCK | O_NOCTTY)) < 0) {
		logprintf(LIRC_ERROR, "Could not open the serial port");
		LOGPRINTF(1, "Could not open the serial port");
		mplay_deinit();
		result = 0;
	}
	/* Serial port configuration */
	else if (!tty_reset(drv.fd) || !tty_setbaud(drv.fd, MPLAY_BAUD_RATE)) {
		logprintf(LIRC_ERROR, "could not configure the serial port for '%s'", drv.device);
		LOGPRINTF(1, "could not configure the serial port for '%s'", drv.device);
		mplay_deinit();
	}
	return result;
}

int mplay2_init(void)
{
	struct termios portset;
	signed int len;
	char buf = 0x96;
	char psResponse[11];

	LOGPRINTF(1, "Entering mplay_init()");
	/* Creation of a lock file for the port */
	if (!tty_create_lock(drv.device)) {
		logprintf(LIRC_ERROR, "Could not create the lock file");
		LOGPRINTF(1, "Could not create the lock file");
		return 0;
	}

	LOGPRINTF(1, "open serial port");
	/* Try to open serial port (Monueal Moncaso 312 device doesn't like O_NONBLOCK */
	if ((drv.fd = open(drv.device, O_RDWR | O_NOCTTY)) < 0) {
		logprintf(LIRC_ERROR, "Could not open the serial port");
		LOGPRINTF(1, "Could not open the serial port");
		tty_delete_lock();
		return 0;
	}

	/* Get serial device parameters */
	if (tcgetattr(drv.fd, &portset) < 0) {
		logprintf(LIRC_ERROR, "Could not get serial port attributes");
		LOGPRINTF(1, "Could not get serial port attributes");
		mplay_deinit();
		return 0;
	}

	/* use own termios struct instead of using tty_reset , Moncaso doesn't like TCSAFLUSH */
	portset.c_cflag &= ~PARENB;
	portset.c_cflag &= ~CSTOPB;
	portset.c_cflag &= ~CSIZE;
	portset.c_cflag = B57600 | CS8;
	portset.c_cflag |= (CLOCAL | CREAD);
	portset.c_iflag |= (IXON | IXOFF | IXANY);
	portset.c_oflag &= ~OPOST;
	portset.c_lflag &= ~(ICANON | ECHOE | ECHO | ISIG);
	portset.c_cc[VSTART] = 0x11;
	portset.c_cc[VSTOP] = 0x13;
	portset.c_cc[VEOF] = 0x20;
	portset.c_cc[VMIN] = 1;
	portset.c_cc[VTIME] = 3;

	if (tcsetattr(drv.fd, TCSANOW, &portset) < 0) {
		logprintf(LIRC_ERROR, "Error setting TCSANOW mode of serial device");
		LOGPRINTF(1, "Error setting TCSANOW mode of serial device");
		mplay_deinit();
		return 0;
	}

	len = write(drv.fd, &buf, 1);
	if (len < 0) {
		LOGPRINTF(1, "couldn't write to device");
		mplay_deinit();
		return 0;
	}

	len = read(drv.fd, &psResponse, 11);
	if (len < 0) {
		LOGPRINTF(1, "No data received during reading");
		mplay_deinit();
		return 0;
	} else
		LOGPRINTF(1, "read chars: %s", psResponse);

	if (tcgetattr(drv.fd, &portset) < 0) {
		logprintf(LIRC_ERROR, "Could not get serial port attributes");
		LOGPRINTF(1, "Could not get serial port attributes");
		mplay_deinit();
		return 0;
	}

	portset.c_cflag &= ~PARENB;
	portset.c_cflag &= ~CSTOPB;
	portset.c_cflag &= ~CSIZE;
	portset.c_cflag = B57600 | CS8;
	portset.c_cflag |= (CLOCAL | CREAD);
	portset.c_iflag |= (IXON | IXOFF | IXANY);
	portset.c_oflag &= ~OPOST;
	portset.c_lflag &= ~(ICANON | ECHOE | ECHO | ISIG);
	portset.c_cc[VSTART] = 0x11;
	portset.c_cc[VSTOP] = 0x13;
	portset.c_cc[VEOF] = 0x1C;
	portset.c_cc[VMIN] = 1;
	portset.c_cc[VTIME] = 3;

	if (tcsetattr(drv.fd, TCSANOW, &portset) < 0) {
		logprintf(LIRC_ERROR, "Error setting TCSANOW mode of serial device");
		LOGPRINTF(1, "Error setting TCSANOW mode of serial device");
		mplay_deinit();
		return 0;
	}

	return 1;
}

/**************************************************************************
 * Close and release the serial line.
 **************************************************************************/
int mplay_deinit(void)
{
	LOGPRINTF(1, "Entering mplay_deinit()");
	close(drv.fd);
	tty_delete_lock();
	drv.fd = -1;
	return (1);
}

/**************************************************************************
 * Receive a code (1 byte) from the remote.
 * This function is called by the LIRC daemon when I/O is pending
 * from a registered client, e.g. irw.
 *
 * return NULL if nothing have been received or a lirc code
 **************************************************************************/
char *mplay_rec(struct ir_remote *remotes)
{
	unsigned char rc_code;
	signed int len;
	struct timeval current_time;
	LOGPRINTF(1, "Entering mplay_rec()");
	len = read(drv.fd, &rc_code, 1);
	gettimeofday(&current_time, NULL);
	if (len != 1) {
		/* Something go wrong during the read, we close the device
		   for prevent endless looping when the device
		   is disconnected */
		LOGPRINTF(1, "Reading error in mplay_rec()");
		mplay_deinit();
		return NULL;
	} else {
		/* We have received a code */
		if (rc_code == MPLAY_REPEAT_CODE) {
			if (mplay_local_data.timeout_repetition_flag == 1) {
				/* We ignore the repetition */
				return NULL;
			} else {
				if (time_elapsed(&mplay_local_data.last_reception_time, &current_time) <=
				    MAX_TIME_BETWEEN_TWO_REPETITION_CODE) {
					/* This reception is a repeat */
					mplay_local_data.repeat_flag = 1;
					/* We save the reception time */
					mplay_local_data.last_reception_time = current_time;
				} else {
					/* To much time between repetition,
					   the receiver have  probably miss
					   a valide key code. We ignore the
					   repetition */
					mplay_local_data.timeout_repetition_flag = 1;
					mplay_local_data.repeat_flag = 0;
					return NULL;
				}
			}
		} else {
			/* This is a new code */
			mplay_local_data.rc_code = rc_code;
			mplay_local_data.repeat_flag = 0;
			mplay_local_data.timeout_repetition_flag = 0;
			mplay_local_data.last_reception_time = current_time;
		}
		LOGPRINTF(1, "code: %u", (unsigned int)mplay_local_data.rc_code);
		LOGPRINTF(1, "repeat_flag: %d", mplay_local_data.repeat_flag);
		return decode_all(remotes);
	}
}

/**************************************************************************
 * This function is called by the LIRC daemon during the transform of a
 * received code into an lirc event.
 *
 * It gets the global variable code (remote keypress code).
 *
 * It returns:
 *  prep                Code prefix (zero for this LIRC driver)
 *  codep               Code of keypress
 *  postp               Trailing code (zero for this LIRC dirver)
 *  repeat_flagp        True if the keypress is a repeated keypress
 *  min_remaining_gapp  Min extimated time gap remaining before next code
 *  max_remaining_gapp  Max extimated time gap remaining before next code
 **************************************************************************/
int mplay_decode(struct ir_remote *remote, struct decode_ctx_t* ctx)
{
	LOGPRINTF(1, "Entering mplay_decode(), code = %u\n", (unsigned int)mplay_local_data.rc_code);

	if (!map_code(remote, ctx, 0, 0, MPLAY_CODE_LENGTH, mplay_local_data.rc_code, 0, 0)) {
		return (0);
	}
	ctx->repeat_flag = mplay_local_data.repeat_flag;
	ctx->min_remaining_gap = 0;
	ctx->max_remaining_gap = 0;
	return 1;
}
