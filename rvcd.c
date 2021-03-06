/*
 * vtgrab - grab the foreground console for display on another machine
 * Copyright (C) 2000  Tim Waugh <twaugh@redhat.com>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * This is the RVC server.
 */

#include <assert.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/vt.h>
#include <sys/kd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include <config.h>
#include "rvc.h"
#include "xfree86.h"

/* set to zero for production */
#define DEBUG 1

static uint8_t features[] = {
	Feature_Crop,
	Feature_Key,
	Feature_IncRectangle,
	Feature_IncScroll,
	Feature_SwitchRequest,
	Feature_Switch,
	Feature_Clear,
};

static uint8_t in_use[256];

static int ready_to_respawn;
static int minor;
static struct vt_stat vtstat;
static int go;

static int vlog (const char *reason, va_list ap)
{
	const size_t chunk = 1000;
	static int allocated;
	static char *buf;
	int needed;

	if (!buf) {
		buf = malloc (chunk);
		if (!buf)
			return 1;

		allocated = chunk;
	}

	for (;;) {
		needed = vsnprintf (buf, allocated, reason, ap);

		if (needed < allocated)
			break;

		free (buf);
		allocated = chunk * ((needed / chunk) + 1);
		buf = malloc (allocated);
		if (!buf)
			return 1;
	}

	/* Maybe one day we'll syslog it */
	fprintf (stderr, "%s", buf);
	return 0;
}

static int log (const char *reason, ...)
{
	va_list vl;
	va_start (vl, reason);
	vlog (reason, vl);
	va_end (vl);
	return 0;
}

static int write_exact (int fd, const void *buffer, size_t len)
{
	const char *buf = buffer;
	while (len) {
		ssize_t put = write (fd, buf, len);
		if (put < 0)
			return put;
		len -= put;
		buf += put;
	}
	fflush (NULL);
	return 0;
}

static int read_exact (int fd, void *buffer, size_t len)
{
	char *buf = buffer;
	while (len) {
		ssize_t got = read (fd, buf, len);
		if (got <= 0)
			return 1;
		len -= got;
		buf += got;
	}
	return 0;
}

static int full_update (int fd, unsigned char *contents, size_t size,
			uint8_t rows, uint8_t cols)
{
	int newfd = dup (fd);
	FILE *f = fdopen (newfd, "r+");
	unsigned char header[6];
	uint16_t content_length = size + 2;
	if (!f)
		return 1;

	header[0] = Msg_IncrementalUpdate;
	header[1] = UpdateType_Rectangle;

	if (in_use[Feature_Crop]) {
		unsigned char *cropped, *p, *q;
		content_length = 4 + (rows * cols * 2);
		cropped = malloc (content_length);
		if (!cropped) {
			fclose (f);
			return 1;
		}
		cropped[0] = rows;
		cropped[1] = cols;
		if (contents[2] < cols)
			cropped[2] = contents[2];
		else
			cropped[2] = cols - 1;
		if ((int)contents[3] > ((int)contents[0] - (int)rows))
			cropped[3] = contents[3] -
				     ((int)contents[0] - (int)rows);
		else
			cropped[3] = 0;
		for (p = cropped + 4; p < cropped + content_length; p += 2) {
			p[0] = ' ';
			p[1] = '\7';
		}
		p = contents + size - 2 * contents[1];
		q = cropped + content_length - 2 * cropped[1];
		if (contents[0] < rows)
			rows = contents[0];
		if (contents[1] < cols)
			cols = contents[1];
		for (; rows; rows--) {
			unsigned char *r = p, *s = q;
			uint8_t i;
			for (i = 0; i < cols; i++) {
				*s++ = *r++;
				*s++ = *r++;
			}
			p -= 2 * contents[1];
			q -= 2 * cropped[1];
		}
		size = content_length;
		content_length += 2; // for offsets
		content_length = htons (content_length);
		memcpy (header + 2, &content_length, 2);
		header[4] = header[5] = 0; // offsets
		fwrite (header, 6, 1, f);
		fwrite (cropped, size, 1, f);
		fclose (f);
		free (cropped);
		return 0;
	}
	content_length = htons (content_length);
	memcpy (header + 2, &content_length, 2);
	header[4] = header[5] = 0; // offsets
	fwrite (header, 6, 1, f);
	fwrite (contents, size, 1, f);
	fclose (f);
	return 0;
}

static int scroll_update (FILE *f, uint8_t lines)
{
	uint8_t message[5];
	message[0] = Msg_IncrementalUpdate;
	message[1] = UpdateType_Scroll;
	message[2] = 0;
	message[3] = 1;
	message[4] = lines;
	fwrite (message, sizeof (message), 1, f);
	return 0;
}

static int clear_update (FILE *f)
{
	uint8_t message[4];
	message[0] = Msg_IncrementalUpdate;
	message[1] = UpdateType_Clear;
	message[2] = 0;
	message[3] = 0;
	fwrite (message, sizeof (message), 1, f);
	return 0;
}

static int incr_update (int fd, unsigned char *last, unsigned char *contents,
			size_t size, uint8_t rows, uint8_t cols)
{
	int newfd;
	FILE *f;
	unsigned char header[10];
	uint16_t content_length;
	uint8_t first_changed, last_changed, i;
	int rowsize = 2 * contents[1];
	static int did_clear_last_time = 0;

	if (in_use[Feature_Crop])
		// Easier just to do a full screen update for this.
		return full_update (fd, contents, size, rows, cols);

	log ("> IncrUpdate\n");
	newfd = dup (fd);
	f = fdopen (newfd, "r+");
	if (!f)
		return 1;

	rows = contents[0];

	// Compare line by line
	for (i = 0; i < rows; i++)
		if (memcmp (contents + 4 + i * rowsize, last + 4 + i * rowsize,
			    rowsize))
			break;

	first_changed = i;
	for (i = rows - 1; i > first_changed; i--)
		if (memcmp (contents + 4 + i * rowsize, last + 4 + i * rowsize,
			    rowsize))
			break;

	last_changed = i;

	// Look for scrolls
	if (in_use[Feature_IncScroll] && contents[3] == rows - 1) {
		uint8_t tryscrl, num;

		for (tryscrl = 1; tryscrl < rows - 1; tryscrl++) {
			const int min = 5;
			uint8_t *old = last + 4 + tryscrl * rowsize;
			uint8_t *new = contents + 4;
			for (num = rows - 1 - tryscrl; num >= min; num--)
				if (!memcmp (old, new, num * rowsize))
					break;
			if (num >= min)
				break;
		}

		// Wahey!
		log ("? Scroll %d: %d, %d\n", tryscrl, first_changed,
		     last_changed);
		if (tryscrl < rows - 1 &&
		    tryscrl < (last_changed - first_changed)) {
			log ("> Scroll %d\n", tryscrl);
			scroll_update (f, tryscrl);
			// Mess with scr so that left-overs get dealt with.
			memcpy (contents + 4, last + 4 + tryscrl * rowsize,
				(rows - tryscrl) * rowsize);
			memset (contents + 4 + (rows - tryscrl) * rowsize, 0,
				tryscrl * rowsize);
			goto out;
		}
	}

	// A clear update might be faster.
	if (in_use[Feature_Clear]) {
		int nonempty = 0, row;
		for (row = 0; row < rows; row++) {
			int c;
			for (c = 0; c < rowsize; c += 2)
				if (contents[4 + row * rowsize + c] != ' ' ||
				    contents[5 + row * rowsize + c] != '\7')
					break;
			if (c < rowsize)
				nonempty++;
		}
		if (!did_clear_last_time &&
		    nonempty < (last_changed - first_changed + 1)) {
			// It's worth our while.
			size_t c;
			log ("> Clear\n");
			did_clear_last_time = 1;
			clear_update (f);
			// Mess with scr so that left-overs get dealt with.
			for (c = 4; c < size; c += 2) {
				contents[c] = ' ';
				contents[c + 1] = '\7';
			}
			goto out;
		}
		did_clear_last_time = 0;
	}

	header[0] = Msg_IncrementalUpdate;
	header[1] = UpdateType_Rectangle;

	// If it's just one line, be smarter.
	if (first_changed == last_changed) {
		uint8_t firstx, lastx;
		uint8_t *cp = contents + 4 + first_changed * rowsize;
		uint8_t *lp = last + 4 + first_changed * rowsize;
		for (firstx = 0; firstx < contents[1]; firstx++)
			if (memcmp (cp + firstx * 2, lp + firstx * 2, 2))
				break;
		for (lastx = contents[1] - 1; lastx > firstx; lastx--)
			if (memcmp (cp + lastx * 2, lp + lastx * 2, 2))
				break;
		size = (lastx - firstx + 1) * 2;
		header[2] = 0;
		header[3] = 6 + (lastx - firstx + 1) * 2;
		header[4] = firstx;
	} else {
		size = (last_changed - first_changed + 1) * rowsize;
		header[4] = 0; // x offset
	}

	content_length = 6 + size;
	content_length = htons (content_length);
	memcpy (header + 2, &content_length, 2);
	header[5] = first_changed; // y offset
	header[6] = last_changed - first_changed + 1;
	header[7] = contents[1];
	header[8] = contents[2];
	header[9] = contents[3];
	fwrite (header, 10, 1, f);
	fwrite (contents + 4 + first_changed * rowsize + 2 * header[4],
		size, 1, f);
out:
	fclose (f);

	return 0;
}

static void skip (int fd, int n)
{
	static unsigned char scratch[256];
	assert (n < 256);
	read_exact (fd, scratch, n);
}

static int do_key (char key)
{
	char tty[16];
	int fd;
	sprintf (tty, "/dev/tty%d", vtstat.v_active);
	fd = open (tty, O_RDONLY | O_NOCTTY);
	ioctl (fd, TIOCSTI, &key);
	close (fd);
	return 0;
}

static int is_a_console (int fd)
{
	char arg = 0;
	return (ioctl(fd, KDGKBTYPE, &arg) == 0
		&& ((arg == KB_101) || (arg == KB_84)));
}

static int open_named_console (const char *tty)
{
	int fd = open (tty, O_RDWR | O_NOCTTY);
	if (fd == -1) {
		fd = open (tty, O_RDONLY | O_NOCTTY);
		if (fd == -1) {
			fd = open (tty, O_WRONLY | O_NOCTTY);
			if (fd == -1)
				return -1;
		}
	}

	if (is_a_console (fd))
		return fd;

	close (fd);
	return -1;
}

static int open_console (void)
{
	int fd = open_named_console ("/dev/tty");
	if (fd == -1)
		fd = open_named_console ("/dev/tty0");
	if (fd == -1)
		fd = open_named_console ("/dev/console");
	return fd;
}

static int do_switch (unsigned char switchmsg)
{
	int cons = switchmsg;
	int fd = open_console ();
	ioctl (fd, VT_ACTIVATE, cons);
	close (fd);
	return 0;
}

static int handle_input (int fd)
{
	uint8_t message_type;
	char keymsg;
	char switchmsg;

	if (read_exact (fd, &message_type, 1)) {
		log ("Problem receiving input\n");
		return -1;
	}

	switch (message_type) {
	case Msg_Terminate:
		log ("< Terminate\n");
		return -1;

	case Msg_FullUpdateRequest:
		log ("< FullUpdateRequest\n");
		return 1;

	case Msg_Key:
		log ("< Key\n");
		if (read (fd, &keymsg, 1) < 1)
			return -1;
		if (!in_use[Feature_Key])
			log ("Not in use!\n");
		else do_key (keymsg);
		break;

	case Msg_SwitchRequest:
		log ("< SwitchRequest\n");
		if (read (fd, &switchmsg, 1) < 1)
			return -1;
		if (!in_use[Feature_SwitchRequest])
			log ("Not in use!\n");
		else do_switch (switchmsg);
		break;

	case Msg_Pointer:
		log ("< Pointer\n");
		skip (fd, 3);
		if (!in_use[Feature_Pointer])
			log ("Not in use!\n");
		break;

	default:
		log ("Invalid message type %d\n", message_type);
		return -1;
	}

	return 0;
}

static int send_switch (int fd, char mode)
{
	unsigned short port;
	uint8_t switchmsg[5];
	if (mode != KD_TEXT)
		vt_switched_to (vtstat.v_active);
	port = port_for_console (vtstat.v_active);
	switchmsg[0] = Msg_Switch;
	switchmsg[1] = vtstat.v_active;
	port = htons (port);
	memcpy (switchmsg + 2, &port, 2);
	switchmsg[4] = mode;
	return write_exact (fd, switchmsg, sizeof (switchmsg));
}

static void handle_sigalrm (int sig __attribute__ ((unused)))
{
	ready_to_respawn = 1;
}

static int server_loop (int fd, uint32_t delay, uint8_t rows, uint8_t cols)
{
	const size_t MAX_CONTENTS = 100000;
	long mode;
	char fgcons[100];
	struct timeval tv;
	unsigned char *contents = malloc (MAX_CONTENTS);
	unsigned char *last = calloc (MAX_CONTENTS, 1);
	int console = open_console ();
	int do_full_update = 1;
	unsigned short last_vt = 0;
	struct sigaction sigalrm;

	sigalrm.sa_handler = handle_sigalrm;
	sigalrm.sa_flags = SA_RESTART;
	sigaction (SIGALRM, &sigalrm, NULL);

	if (console == -1) {
		perror ("no console access");
		exit (1);
	}

	/* Minimum 50ms between updates */
	if (delay < 50)
		delay = 50;
	/* Cap delay at a sensible amount */
	if (delay > 100000)
		delay = 100000;

	if (!contents || !last) {
		printf ("Memory squeeze\n");
		exit (1);
	}

	ready_to_respawn = 1;
	for (;;) {
		fd_set readfds;
		ssize_t size = 0;
		sigset_t set;
		int c;

		FD_ZERO (&readfds);
		FD_SET (fd, &readfds);
		tv.tv_sec = 0;
		tv.tv_usec = delay * 1000;
		switch (select (fd + 1, &readfds, NULL, NULL, &tv)) {
		case 0: // timeout
			break;

		case -1: // probably interrupted
			break;

		default:
			if (FD_ISSET (fd, &readfds)) {
				switch (handle_input (fd)) {
				case 0:
					continue;
				case 1:
					do_full_update = 1;
					break;
				default:
					goto out;
				}
			}
		}

		sigemptyset (&set);
		sigaddset (&set, SIGCHLD);
		sigaddset (&set, SIGALRM);
		sigprocmask (SIG_BLOCK, &set, NULL);
		while (reapees) {
			reap_child ();
			reapees--;
		}

		if (ready_to_respawn) {
			do_respawn ();
			ready_to_respawn = 0;
			alarm (5);
		}

		sigprocmask (SIG_UNBLOCK, &set, NULL);
	
		if (ioctl (console, VT_GETSTATE, &vtstat)) {
			perror ("VT_GETSTATE");
			close (console);
			console = open_console ();
			continue;
			exit (1);
		}
		sprintf (fgcons, "/dev/tty%d", vtstat.v_active);
		c = open (fgcons, O_RDONLY | O_NOCTTY);
		if (c == -1) {
			perror (fgcons);
			exit (1);
		}
		if (ioctl (c, KDGETMODE, &mode)) {
			perror ("KDGETMODE");
			exit (1);
		}
		close (c);
		sprintf (fgcons, "/dev/vcsa%d", vtstat.v_active);

		if (in_use[Feature_Switch]
		    && (last_vt != vtstat.v_active))
			send_switch (fd, mode);

		if (mode == KD_TEXT) {
			c = open (fgcons, O_RDONLY | O_NOCTTY);
			if (c == -1) {
				perror (fgcons);
				exit (1);
			}
			size = read (c, contents, MAX_CONTENTS);
			close (c);

			if (do_full_update) {
				full_update (fd, contents, size, rows, cols);
				memcpy (last, contents, size);
			} else if (memcmp (last, contents, size)) {
				incr_update (fd, last, contents, size, rows,
					     cols);
				memcpy (last, contents, size);
			}
		} else if (size) {
			size = 0;
			memset (last, 0, MAX_CONTENTS);
		}

		go = 1;
		do_full_update = 0;
		last_vt = vtstat.v_active;
	}

out:
	close (console);

	/**
	 * Drain the input
	 **/
	sleep (1);
	if (isatty (fd))
		tcflush (fd, TCIOFLUSH);
	else {
		int flags;
		fcntl (fd, F_GETFL, &flags);
		flags |= O_NONBLOCK;
		fcntl (fd, F_SETFL, &flags);
		while (read (fd, contents, MAX_CONTENTS) > 0)
			;
		flags &= ~O_NONBLOCK;
		fcntl (fd, F_SETFL, &flags);
	}

	return 0;
}

static int server (int fd)
{
	const size_t protverlen = 12;
	char protocol_version[13];
	unsigned long our_major, their_major;
	uint32_t auth;
	uint8_t num_features, i;
	char *p, *end, *buf;
	struct ClientInitialisation_fixedpart ci;

	/**
	 * Send ProtocolVersion
	 **/
	if (write_exact (fd, RVC_PROTOCOL_VERSION, protverlen))
		return log ("Problem sending ProtocolVersion\n");

	/**
	 * Receive ProtocolVersion
	 **/
	memset (protocol_version, 0, protverlen + 1);
	if (read_exact (fd, protocol_version, protverlen))
		return log ("Problem receiving ProtocolVersion\n");

	p = protocol_version + 4;
 try_again:
	their_major = strtoul (p, &end, 10);
	if (p == end || strncmp (protocol_version, RVC_PROTOCOL_VERSION, 4) ||
	    protocol_version[7] != '.' ||
	    protocol_version[protverlen - 1] != '\n') {
		unsigned int i;
		log ("Garbled communications: \"%s\"\n", protocol_version);
		for (i = 1; i < protverlen - 1; i++) {
			size_t match = protverlen - i;
			if (match > 4)
				match = 4;
			if (!strncmp (protocol_version + i,
				      RVC_PROTOCOL_VERSION, 4)) {
				memmove (protocol_version,
					 protocol_version + i, protverlen - i);
				if (read_exact (fd, protocol_version
						+ protverlen - i, i))
					break;
				log ("Restarting with: \"%s\"\n",
				     protocol_version);
				goto try_again;
			}
		}
		return log ("Garbled ProtocolVersion\n");
	}

	p = RVC_PROTOCOL_VERSION + 4;
	our_major = strtoul (p, &end, 10);
	assert (p != end);

	p = protocol_version + 8;
	minor = strtoul (p, &end, 10);
	if (p == end)
		return log ("Garbled ProtocolVersion\n");

	if (our_major != their_major)
		return log ("Incompatible ProtocolVersion\n");

	log ("Using ProtocolVersion %s", protocol_version);

	/**
	 * Send Authentication
	 **/
	auth = htonl (AuthNoAuth);
	if (write_exact (fd, &auth, 4))
		return log ("Problem sending Authentication\n");


	/**
	 * ServerInitialisation
	 **/
	num_features = sizeof (features);
	if (write_exact (fd, &num_features, 1))
		return log ("Problem sending ServerInitialisation\n");
	if (write_exact (fd, features, num_features))
		return log ("Problem sending ServerInitialisation\n");

	/**
	 * ClientInitialisation
	 **/
	if (read_exact (fd, &ci, sizeof (ci)))
		return log ("Problem receiving ClientInitialisation\n");
	log ("updatems: %d\n", ci.updatems);
	log ("rows: %d\n", ci.rows);
	log ("cols: %d\n", ci.cols);
	buf = malloc (ci.num_features);
	if (!buf)
		return log ("Out of memory while receiving "
			    "ClientInitialisation\n");
	if (read_exact (fd, buf, num_features)) {
		free (buf);
		return log ("Problem receiving feature list\n");
	}

	memset (in_use, 0, sizeof (in_use));
	for (i = 0; i < sizeof (features); i++) {
		if (buf[i]) {
			in_use[features[i]] = 1;
			log ("In use: %d\n", features[i]);
		}
	}

	return server_loop (fd, ci.updatems, ci.rows, ci.cols);
}

static void syntax (void)
{
	fprintf (stderr, "Usage: rvcd :<port>\n"
			 "       rvcd <tty>\n"
			 "       rvcd --help\n"
			 "       rvcd --syntax\n");
}

int main (int argc, char **argv)
{
	int fd;
	int s = -1;

	if (argc < 2) {
		syntax ();
		exit (1);
	}

	if (!strcmp (argv[1], "--help")) {
		syntax ();
		exit (0);
	}

	if (!strcmp (argv[1], "--version")) {
		printf ("RVCD (vtgrab %s)\n", VERSION);
		exit (0);
	}
	
	if ((fd = open (argv[1], O_RDWR | O_NOCTTY)) == -1) {
		struct sockaddr_in sin;
		unsigned long port;
		char *start, *end;
		int on = 1;
		if (argv[1][0] != ':') {
			perror (argv[1]);
			exit (1);
		}
		start = argv[1] + 1;
		port = strtoul (start, &end, 10);
		if (start == end) {
			syntax ();
			exit (1);
		}
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = INADDR_ANY;
		sin.sin_port = htons (port);
		s = socket (PF_INET, SOCK_STREAM, 0);
		if (s < 0) {
			perror ("socket");
			exit (1);
		}
		setsockopt (s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof (on));
		if (bind (s, (struct sockaddr *) &sin, sizeof (sin))) {
			perror ("bind");
			close (s);
			exit (1);
		}
		if (listen (s, 1)) {
			perror ("listen");
			close (s);
		}
		fd = accept (s, NULL, NULL);
		if (fd == -1) {
			perror ("accept");
			close (s);
			exit (1);
		}
	} else if (isatty (fd)) {
		/* Need to set raw mode */
		struct termios tios;
		tcgetattr (fd, &tios);
		cfmakeraw (&tios);
		cfsetospeed (&tios, B9600);
		cfsetispeed (&tios, B9600);
		tcsetattr (fd, TCSANOW, &tios);
		tcflush (fd, TCIOFLUSH);
	}

	xfree86_init ();
	for (;;) {
		server (fd);
		if (s != -1) {
			fd = accept (s, NULL, NULL);
			if (fd == -1) {
				perror ("accept");
				close (s);
				exit (1);
			}
		}
	}

	return 0;
}
