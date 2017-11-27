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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/vt.h>
#include <sys/kd.h>
#include <sys/time.h>

#define VERSION "0.0.1"

static int server (void)
{
	const size_t MAX_CONTENTS = 100000;
	struct vt_stat vtstat;
	long mode;
	char fgcons[100];
	struct timeval tv;
	char *contents = malloc (MAX_CONTENTS);
	char *last = calloc (MAX_CONTENTS, 1);
	if (!contents || !last) {
		printf ("Memory squeeze\n");
		exit (1);
	}
	for (;;) {
		ssize_t size;
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
		if (mode == KD_TEXT) {
			c = open (fgcons, O_RDONLY | O_NOCTTY);
			if (c == -1) {
				perror (fgcons);
				exit (1);
			}
			size = read (c, contents, MAX_CONTENTS);
			close (c);
		} else if (size) {
			size = 0;
			memset (last, 0, MAX_CONTENTS);
			printf ("%ld,0\n", mode);
			fflush (stdout);
		}

		if (memcmp (last, contents, size)) {
			printf ("%ld,%dz\n", mode, size);

			fwrite (contents, size, 1, stdout);
			fflush (stdout);
			memcpy (last, contents, size);
		}

		tv.tv_sec = 0;
		tv.tv_usec = 50000;
		select (0, NULL, NULL, NULL, &tv);
	}

	return 0;
}

static int client (void)
{
	struct vt_stat vtstat;
	char fgcons[100];
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

	for (;;) {
		long mode;
		ssize_t len;
		FILE *f;
		scanf ("%ld,%dz\n", &mode, &len);
		f = fopen (fgcons, "w+");
		if (!f) {
			perror (fgcons);
			exit (1);
		}
		if (mode) {
			unsigned char buffer[4];
			const char *message = "Can't grab VT with mode != 0";
			fread (buffer, 1, 4, f);
			while (*message) {
				fputc (*message, f);
				fputc (64, f);
				message++;
			}
		} else {
			while (len--) {
				int ch = getchar ();
				if (ch == EOF) break;
				fputc (ch, f);
			}
		}
		fclose (f);
	}
}

static void syntax (void)
{
	printf ("usage: vtgrab --version\n"
		"       vtgrab --help\n"
		"       vtgrab --server\n"
		"       vtgrab --client\n");
}

int main (int argc, char **argv)
{
	if (argc != 2) {
		syntax ();
		exit (1);
	}

	if (!strcmp (argv[1], "--version")) {
		printf ("vtgrab %s\n", VERSION);
		exit (0);
	}

	if (!strcmp (argv[1], "--help")) {
		syntax ();
		exit (0);
	}

	if (!strcmp (argv[1], "--server"))
		return server ();

	if (!strcmp (argv[1], "--client"))
		return client ();

	syntax ();
	exit (1);
}
