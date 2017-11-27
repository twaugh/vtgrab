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
 */

#include <assert.h>
#include <config.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/vt.h>
#include <sys/kd.h>
#include <sys/time.h>
#include <termios.h>

#include "rvc.h"

#define KEY 1

/* Features we want to make use of, if offered */
static uint8_t wishlist[] = {
	Feature_Crop,
#if KEY
	Feature_Key,
#endif
};

static uint8_t in_use[256];

static struct termios saved_attributes;

static void reset_input_mode (void)
{
	tcsetattr (STDIN_FILENO, TCSANOW, &saved_attributes);
}

static void set_input_mode (void)
{
	struct termios tattr;

	/* Make sure stdin is a terminal. */
	if (!isatty (STDIN_FILENO)) {
		fprintf (stderr, "Not a terminal.\n");
		exit (1);
	}

	/* Save the terminal attributes so we can restore them later. */
	tcgetattr (STDIN_FILENO, &saved_attributes);
	atexit (reset_input_mode);

	/* Set the funny terminal mode. */
	tcgetattr (STDIN_FILENO, &tattr);
	tattr.c_lflag &= ~(ICANON|ECHO); /* Clear ICANON and ECHO. */
	tattr.c_cc[VMIN] = 1;
	tattr.c_cc[VTIME] = 0;
	tcsetattr (STDIN_FILENO, TCSAFLUSH, &tattr);
}

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

static void skip (int fd, int n)
{
	static unsigned char scratch[256];
	assert (n < 256);
	read_exact (fd, scratch, n);
}

static int send_key (int fd)
{
	unsigned char keymsg[2];
	keymsg[0] = Msg_Key;
	if (read (STDIN_FILENO, &keymsg[1], 1) < 1)
		return 1;
	write_exact (fd, keymsg, sizeof (keymsg));
	return 0;
}

static int client_loop (int fd, FILE *f)
{
	unsigned char *contents = NULL;
	int allocated = 0;

	for (;;) {
		uint8_t message_type;
		uint8_t update_type;
		uint16_t len;
		fd_set readfds;

		rewind (f);
		FD_ZERO (&readfds);
		FD_SET (fd, &readfds);
		if (in_use[Feature_Key])
			FD_SET (STDIN_FILENO, &readfds);
		select (fd + 1, &readfds, NULL, NULL, NULL);
		if (FD_ISSET (STDIN_FILENO, &readfds)) {
			send_key (fd);
		}
		if (!FD_ISSET (fd, &readfds))
			continue;
		if (read_exact (fd, &message_type, 1)) {
			exit (0);
		}

		switch (message_type) {
		case Msg_IncrementalUpdate:
			//log ("< IncrementalUpdate\n");
			break;

		case Msg_Switch:
			log ("< Switch\n");
			if (!in_use[Feature_Switch])
				log ("Not in use: Switch\n");
			skip (fd, 4);
			continue;
		}

		read_exact (fd, &update_type, 1);
		read_exact (fd, &len, 2);
		len = ntohs (len);

		if (allocated < len) {
			if (contents)
				free (contents);
			contents = malloc (len);
			allocated = len;
		}
		if (!contents)
			exit (1);

		read_exact (fd, contents, len);

		if (contents[0] == 0 && contents[1] == 0)
			fwrite (contents + 2, 1, len - 2, f);
		else if (!in_use[Feature_IncRectangle])
			log ("Not in use: Incremental Rectangle Updates\n");
		else
			log ("Not implemented: Incremental Rectangle Updates\n");

		fflush (f);
	}
}

static int client (int fd)
{
	const size_t protverlen = 12;
	char protocol_version[13];
	uint32_t auth;
	uint8_t num_features, i;
	char *p, *end, *features;
	unsigned long their_major;
	struct ClientInitialisation_fixedpart ci;
	struct vt_stat vtstat;
	char fgcons[100];
	FILE *vcsa;
	int c = open ("/dev/console", O_RDONLY | O_NOCTTY);
	if (c == -1) {
		perror ("/dev/console");
		exit (1);
	}
	if (ioctl (c, VT_GETSTATE, &vtstat)) {
		perror ("VT_GETSTATE");
		exit (1);
	}
	close (c);
	sprintf (fgcons, "/dev/vcsa%d", vtstat.v_active);
	vcsa = fopen (fgcons, "r+");
	if (!vcsa) {
		fprintf (stderr, "Error opening %s\n", fgcons);
		exit (1);
	}

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

	/**
	 * Send ProtocolVersion
	 **/
	if (write_exact (fd, RVC_PROTOCOL_VERSION, protverlen))
		return log ("Problem sending ProtocolVersion\n");

	/**
	 * Receive Authentication
	 **/
	if (read_exact (fd, &auth, 4))
		return log ("Problem receiving Authentication\n");
	auth = ntohl (auth);
	if (auth != 1)
		return log ("Connection problem\n");

	/**
	 * Receive ServerInitialisation
	 **/
	if (read_exact (fd, &num_features, 1))
		return log ("Problem receiving ServerInitialisation\n");
	features = malloc (num_features);
	if (!features) {
		free (features);
		return log ("Out of memory during ServerInitialisation\n");
	}
	if (read_exact (fd, features, num_features))
		return log ("Problem receiving feature list\n");

	/**
	 * Send ClientInitialisation
	 **/
	ci.updatems = 0;
	ci.rows = 25;
	ci.cols = 80;
	fread (&ci.rows, 1, 1, vcsa);
	fread (&ci.cols, 1, 1, vcsa);
	ci.pad1 = ci.pad2 = ci.pad3 = ci.pad4 = 0;
	ci.num_features = num_features;
	if (write_exact (fd, &ci, sizeof (ci)))
		return log ("Problem sending ClientInitialisation\n");
	for (i = 0; i < num_features; i++) {
		uint8_t feature = features[i];
		unsigned int j;
		features[i] = 0;
		for (j = 0; j < sizeof (wishlist); j++) {
			if (wishlist[j] == feature) {
				features[i] = 1;
				log ("In use: %d\n", feature);
				break;
			}
		}
		in_use[feature] = features[i];
	}
	if (write_exact (fd, features, num_features))
		return log ("Problem sending num-features\n");

	if (in_use[Feature_Key])
		set_input_mode ();

	return client_loop (fd, vcsa);
}

static void syntax (void)
{
	printf ("Usage: rvc <host>:<port>\n"
		"       rvc <tty>\n"
		"	rvc --version\n"
		"       rvc --help\n");
}

static int connect_to (const char *name)
{
	char *host, *p, *end;
	struct hostent *h;
	struct sockaddr_in sin;
	unsigned long port;
	int s;

	host = strdup (name);
	if (!host) {
		fprintf (stderr, "Out of memory\n");
		exit (1);
	}
	p = strrchr (host, ':');
	if (!p) {
		syntax ();
		exit (1);
	}
	*p++ = '\0';
	if (!*host) {
		syntax ();
		exit (1);
	}
	h = gethostbyname (host);
	if (!h || !*h->h_addr_list) {
		fprintf (stderr, "unknown host %s\n", host);
		exit (1);
	}
	memcpy (&sin.sin_addr.s_addr, *h->h_addr_list, h->h_length);
	port = strtoul (p, &end, 10);
	if (p == end) {
		syntax ();
		exit (1);
	}

	sin.sin_port = htons (port);
	sin.sin_family = AF_INET;
	s = socket (PF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		perror ("socket");
		exit (1);
	}
	if (connect (s, &sin, sizeof (sin)) < 0) {
		perror ("connect");
		exit (1);
	}
	return s;
}

int main (int argc, char **argv)
{
	int fd;

	if (argc < 2) {
		syntax ();
		exit (1);
	}

	if (!strcmp (argv[1], "--version")) {
		printf ("RVC (vtgrab %s)\n", VERSION);
		exit (0);
	}

	if (!strcmp (argv[1], "--help")) {
		syntax ();
		exit (0);
	}

	if ((fd = open (argv[1], O_RDWR)) == -1)
		fd = connect_to (argv[1]);

	return client (fd);
}
