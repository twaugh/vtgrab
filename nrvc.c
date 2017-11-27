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
 * This is an RVC client, based on ncurses.
 */
 
#include <assert.h>
#include <config.h>
#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rvc.h"
#include "simple_panel.h"

#define DEBUG 0

static uint8_t wishlist[] = {
	Feature_Key,
	Feature_IncRectangle,
	Feature_SwitchRequest,
	Feature_Switch,
};

static int caught_break = 0;
static int colourmap[8][8];
static char *remote_host = NULL;
static uint8_t in_use[256];
static int keyboard_control = 0;
static int (*menu) (int fd, int key) = NULL;
static PANEL *status_pnl = NULL;
static int status_pnl_expired;
static PANEL *display = NULL;
static int display_pminrow, display_pmincol;

static int log (char *fmt __attribute__ ((unused)), ...)
{
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

static int send_key (int fd, char key)
{
	int xfd = dup (fd);
	FILE *f = fdopen (xfd, "r+");
	if (!f)
		return 1;

	fputc (Msg_Key, f);
	fputc (key, f);
	fclose (f);
	return 0;
}

static int request_full_update (int fd)
{
	uint8_t msg = Msg_FullUpdateRequest;
	return write_exact (fd, &msg, 1);
}

static int send_switchrequest (int fd, int switchto)
{
	int xfd = dup (fd);
	FILE *f = fdopen (xfd, "r+");
	if (!f)
		return 1;

	fputc (Msg_SwitchRequest, f);
	fputc (switchto, f);
	fclose (f);
	return request_full_update (fd);
}

static int vt_menu (int fd, int key)
{
	static WINDOW *wnd;
	static PANEL *pnl;
	int height = 3, width = 25;
	int switchto = -1;

	if (!wnd) {
		int startx, starty;
		int y, x;
		getmaxyx (stdscr, y, x);
		starty = (y - height) / 2;
		startx = (x - width) / 2;
		wnd = newwin (height, width, starty, startx);
		if (!wnd)
			return 1;

		pnl = new_panel (wnd);
		box (wnd, ACS_VLINE, ACS_HLINE);
		y = 1;
		wmove (wnd, y, 2);
		wprintw (wnd, "Press a function key");
		wmove (wnd, y, width - 2);
		update_panels ();
		doupdate ();
		return 0;
	}

	switch (key) {
	case '\033':
		switchto = 0;
		break;
	default:
		if (key > KEY_F0 && key < KEY_F0 + 64)
			switchto = key - KEY_F0;
		else if (key > '0' && key <= '9')
			switchto = key - '0';
		else {
			beep ();
			return 0;
		}
		break;
	}

	del_panel (pnl);
	delwin (wnd);
	pnl = NULL;
	wnd = NULL;
	menu = NULL;
	update_panels ();
	doupdate ();
	if (!switchto)
		return 0;

	send_switchrequest (fd, switchto);
	return 0;
}

static int main_menu (int fd, int key)
{
	static WINDOW *wnd;
	static PANEL *pnl;
	static int selection;
	int height = 9, width = 28, nselect = 5;

	if (!wnd) {
		int startx, starty;
		int y, x;
		getmaxyx (stdscr, y, x);
		starty = (y - height) / 2;
		startx = (x - width) / 2;
		wnd = newwin (height, width, starty, startx);
		if (!wnd)
			return 1;

		pnl = new_panel (wnd);
		box (wnd, ACS_VLINE, ACS_HLINE);
		y = 2;
		wmove (wnd, y++, 3);
		wprintw (wnd, "Send Control-");
		wattron (wnd, A_UNDERLINE);
		wprintw (wnd, "A");
		wattroff (wnd, A_UNDERLINE);
		wmove (wnd, y++, 3);
		wattron (wnd, A_UNDERLINE);
		wprintw (wnd, "E");
		wattroff (wnd, A_UNDERLINE);
		wprintw (wnd, "xit main menu");
		wmove (wnd, y, 3);
		wattron (wnd, A_UNDERLINE);
		wprintw (wnd, "K");
		wattroff (wnd, A_UNDERLINE);
		wprintw (wnd, "eyboard control");
		wmove (wnd, y++, width - 5);
		wprintw (wnd, "%s", keyboard_control ? " On" : "Off");
		wmove (wnd, y++, 3);
		wprintw (wnd, "Switch ");
		wattron (wnd, A_UNDERLINE);
		wprintw (wnd, "v");
		wattroff (wnd, A_UNDERLINE);
		wprintw (wnd, "irtual terminal");
		wmove (wnd, y++, 3);
		wprintw (wnd, "E");
		wattron (wnd, A_UNDERLINE);
		wprintw (wnd, "x");
		wattroff (wnd, A_UNDERLINE);
		wprintw (wnd, "it viewer");
		selection = 0;
		wmove (wnd, 2 + selection, 2);
		wprintw (wnd, ">");
		wmove (wnd, 2 + selection, width - 2);
		update_panels ();
		doupdate ();
		return 0;
	}

	switch (key) {
	case KEY_UP:
		wmove (wnd, 2 + selection, 2);
		wprintw (wnd, " ");
		selection--;
		selection += nselect;
		selection %= nselect;
		wmove (wnd, 2 + selection, 2);
		wprintw (wnd, ">");
		wmove (wnd, 2 + selection, width - 2);
		break;
	case KEY_DOWN:
		wmove (wnd, 2 + selection, 2);
		wprintw (wnd, " ");
		selection++;
		selection %= nselect;
		wmove (wnd, 2 + selection, 2);
		wprintw (wnd, ">");
		wmove (wnd, 2 + selection, width - 2);
		break;
	case '\001':
	case '\033':
	case 'a':
	case 'e':
	case 'k':
	case 'v':
	case 'x':
		switch (key) {
		case '\001':
		case 'a':
			selection = 0;
			break;
		case '\033':
		case 'e':
			selection = 1;
			break;
		case 'k':
			selection = 2;
			break;
		case 'v':
			selection = 3;
			break;
		case 'x':
			selection = 4;
			break;
		}
		// fall-through
	case '\r':
	case '\n':
		del_panel (pnl);
		delwin (wnd);
		pnl = NULL;
		wnd = NULL;
		menu = NULL;
		switch (selection) {
		case 0: // Send control-A
			send_key (fd, '\001');
			break;
		case 1: // Exit main menu
			break;
		case 2: // Keyboard control
			keyboard_control = !keyboard_control;
			break;
		case 3:
			menu = vt_menu;
			(*menu) (fd, key);
			break;
		case 4: // Exit viewer
			update_panels ();
			doupdate ();
			exit (0);
		}
		break;
	}

	update_panels ();
	doupdate ();
	return 0;
}

static void update_display_panel (void)
{
	int slop;
	WINDOW *wnd = panel_window (display);
	int curx, cury, maxx, maxy, pmaxx, pmaxy;
	getyx (wnd, cury, curx);
	getmaxyx (wnd, pmaxy, pmaxx);
	getmaxyx (stdscr, maxy, maxx);
	slop = maxy / 3;
	if (curx < display_pmincol) {
		display_pmincol = curx - slop;
		if (display_pmincol < 0)
			display_pmincol = 0;
	}
	if (curx >= (display_pmincol + maxx)) {
		display_pmincol = curx - maxx + slop;
		if ((display_pmincol + maxx) > pmaxx)
			display_pmincol = pmaxx - maxx;
	}
	if (cury < display_pminrow) {
		display_pminrow = cury - slop;
		if (display_pminrow < 0)
			display_pminrow = 0;
	}
	if (cury >= (display_pminrow + maxy)) {
		display_pminrow = cury - maxy + slop;
		if ((display_pminrow  + maxy) > pmaxy)
			display_pminrow = pmaxy - maxy;
	}
	panel_is_pad (display, display_pminrow, display_pmincol, 0, 0,
		      maxy, maxx);
}

static int create_display_panel (int lines, int cols)
{
	WINDOW *wnd = newpad (lines, cols);
	if (!wnd)
		return 1;

	werase (wnd);
	display = new_panel (wnd);
	if (!display)
		return 1;

	bottom_panel (display);
	display_pmincol = 0;
	display_pminrow = 0;
	return 0;
}

static int handle_update (int fd)
{
	static uint8_t *contents = NULL;
	static uint16_t allocated = 0;
	WINDOW *wnd;
	uint8_t header[3];
	uint8_t update_type;
	uint16_t content_length;
	uint8_t x, y;

	if (read_exact (fd, header, 3))
		return 1;

	update_type = header[0];
	memcpy (&content_length, &header[1], 2);
	content_length = ntohs (content_length);

	if (allocated < content_length) {
		if (contents)
			free (contents);
		allocated = content_length;
		contents = malloc (allocated);
		if (!contents) {
			allocated = 0;
			return 1;
		}
	}

	if (read_exact (fd, contents, content_length))
		return 1;

	if (update_type != UpdateType_Rectangle)
		return 1;

	if (!contents[0] && !contents[1]) {
		if (display) {
			int y, x;
			wnd = panel_window (display);
			getmaxyx (wnd, y, x);
			if (contents[2] > y || contents[3] > x) {
				wresize (wnd, contents[2], contents[3]);
				werase (wnd);
				wmove (wnd, 0, 0);
			}
		} else {
			if (create_display_panel (contents[2], contents[3]))
				return 1;
		}
	}

	wnd = panel_window (display);

	for (y = 0; y < contents[2]; y++) {
		uint8_t *p;
		p = contents + 6;
		p += y * 2 * contents[3];
		for (x = 0; x < contents[3]; x++) {
			uint8_t attrbyte = p[2 * x + 1];
			int attr = 0;
			int fg, bg;
			if (attrbyte & 0x80)
				attr |= A_BLINK;
			if (attrbyte & 0x08)
				attr |= A_BOLD;
			fg = attrbyte & 7;
			bg = (attrbyte >> 4) & 7;
			attr |= COLOR_PAIR (colourmap[bg][fg]);
			wattrset (wnd, attr);
			mvwaddch (wnd, y + contents[1], x + contents[0],
				  p[2 * x]);
		}
	}

	wattrset (wnd, A_NORMAL);
	wmove (wnd, contents[5], contents[4]);
	update_display_panel ();
	update_panels ();
	doupdate ();

	return 0;
}

static void sigalrm (int sig __attribute__ ((unused)))
{
	// Clean up status bar
	status_pnl_expired = 1;
}

static void sigint (int sig __attribute__ ((unused)))
{
	caught_break++;
}

static pid_t try_vncviewer (unsigned short port)
{
	char *param;
	pid_t pid;
	int null;
	if (!remote_host)
		return 1;
	param = malloc (strlen (remote_host) + 20);
	if (!param)
		return 1;
	sprintf (param, "%s:%d", remote_host, port - 5900);
	null = open ("/dev/null", O_RDWR);
	pid = fork ();
	if (pid == -1)
		return pid;
	if (!pid) {
		// Child.
		close (0);
		close (1);
		close (2);
		dup (null);
		dup (null);
		dup (null);
		execlp ("vncviewer", "vncviewer", param, NULL);
		exit (1);
	}
	close (null);
	free (param);
	return pid;
}

static int handle_switch (int fd)
{
	static int first = 1;
	static pid_t vncviewer = -1;
	WINDOW *wnd;
	uint8_t switchmsg[4];
	uint16_t port;
	char status_bar[100];
	int len;

	if (read_exact (fd, switchmsg, sizeof (switchmsg)))
		return 1;

	memcpy (&port, &switchmsg[1], 2);
	port = ntohs (port);
	if (first) {
		first = 0;
		strcpy (status_bar, "Press Control-A for menu");
	} else
		sprintf (status_bar, "tty%d (mode %d)", (int) switchmsg[0],
			 (int) switchmsg[3]);
	len = strlen (status_bar);

	if (status_pnl) {
		wnd = panel_window (status_pnl);
		del_panel (status_pnl);
		delwin (wnd);
		status_pnl = NULL;
	}

	wnd = newwin (1, len, 0, 0);
	status_pnl = new_panel (wnd);
	wattron (wnd, A_REVERSE);
	mvwprintw (wnd, 0, 0, "%s", status_bar);
	curs_set (0);

	if (vncviewer != -1) {
		switch (waitpid (vncviewer, NULL, WNOHANG)) {
		case 0: // still running
			kill (vncviewer, SIGTERM);
			waitpid (vncviewer, NULL, 0);
			vncviewer = -1;
			break;
		default: // exited
		case -1: // wrong pid
			vncviewer = -1;
			break;
		}
	}

	if (switchmsg[3]) {
		WINDOW *dwnd = panel_window (display);
		werase (dwnd);
		mvwprintw (dwnd, 2, 2, "%s",
			  "It is not possible to display this terminal "
			  "as text.");
		if (port) {
			mvwprintw (dwnd, 3, 2,
				   "Use vncviewer to view display :%d.",
				   port - 5900);
			vncviewer = try_vncviewer (port);
		}

		wmove (dwnd, 0, 0);
		update_display_panel ();
	}

	update_panels ();
	doupdate ();
	status_pnl_expired = 0;
	alarm (2);

	if (!switchmsg[3])
		request_full_update (fd);

	return 0;
}

static int handle_input (int fd)
{
	if (menu) {
		int key = getch();
		(*menu) (fd, key);
	} else {
		char key;
		if (read_exact (STDIN_FILENO, &key, 1))
			return 1;
		if (key == '\001') {
			menu = main_menu;
			(*menu) (fd, key);
		} else {
			if (keyboard_control)
				send_key (fd, key);
			return 0;
		}
	}

	return 0;
}

static int client_loop (int fd)
{
	int ret = 0;

	for (;;) {
		unsigned char msg;
		struct timeval tv;
		fd_set readfds;
		int sretcode;

		if (status_pnl_expired) {
			if (status_pnl) {
				WINDOW *wnd = panel_window (status_pnl);
				del_panel (status_pnl);
				delwin (wnd);
				curs_set (1);
				update_panels ();
				doupdate ();
			}
			status_pnl = NULL;
			status_pnl_expired = 0;
		}

		// Clean up any vncviewers that are lying around
		waitpid (-1, NULL, WNOHANG);

		FD_ZERO (&readfds);
		FD_SET (STDIN_FILENO, &readfds);
		FD_SET (fd, &readfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		sretcode = select (fd + 1, &readfds, NULL, NULL, &tv);
		if (!sretcode || (sretcode == -1 && errno == EINTR)) {
			/* Timeout. */
			sigset_t set;
			sigemptyset (&set);
			sigaddset (&set, SIGINT);
			sigprocmask (SIG_BLOCK, &set, NULL);
			while (caught_break) {
				if (keyboard_control)
					send_key (fd, '\3');
				caught_break--;
			}
			sigprocmask (SIG_UNBLOCK, &set, NULL);
			continue;
		}

		if (FD_ISSET (STDIN_FILENO, &readfds))
			handle_input (fd);
		if (!(FD_ISSET (fd, &readfds)))
			continue;

		/* Now handle input from the RVC server. */
		if (read_exact (fd, &msg, 1)) {
			ret = 1;
			break;
		}

		if (msg == Msg_Terminate)
			break;

		switch (msg) {
		case Msg_IncrementalUpdate:
			handle_update (fd);
			break;

		case Msg_Switch:
			handle_switch (fd);
			break;

		default:
			log ("Unknown message type 0x%02x\n", msg);
			break;
		}
	}

	return 0;
}

static void do_colourmap_setup (int high, int background)
{
	int i;
	for (i = 0; i < 7; i++)
		colourmap[high][i] = high * 8 + i;

	colourmap[high][7] = 0;

	init_pair (colourmap[high][0x0], COLOR_BLACK, background);
	init_pair (colourmap[high][0x1], COLOR_BLUE, background);
	init_pair (colourmap[high][0x2], COLOR_GREEN, background);
	init_pair (colourmap[high][0x3], COLOR_CYAN, background);
	init_pair (colourmap[high][0x4], COLOR_RED, background);
	init_pair (colourmap[high][0x5], COLOR_MAGENTA, background);
	init_pair (colourmap[high][0x6], COLOR_YELLOW, background);
}

void setup_colours (void)
{
	start_color ();
	assert (COLOR_PAIRS >= (8 * 8));

	do_colourmap_setup (0, COLOR_BLACK);
	do_colourmap_setup (1, COLOR_BLUE);
	do_colourmap_setup (2, COLOR_GREEN);
	do_colourmap_setup (3, COLOR_CYAN);
	do_colourmap_setup (4, COLOR_RED);
	do_colourmap_setup (5, COLOR_MAGENTA);
	do_colourmap_setup (6, COLOR_YELLOW);
	do_colourmap_setup (7, COLOR_CYAN);
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
	struct sigaction alrm;
	struct sigaction sint;

	initscr ();
	atexit ((void(*)(void))endwin);
	cbreak ();
	noecho ();
	nonl ();
	intrflush (stdscr, FALSE);
	keypad (stdscr, TRUE);
	setup_colours ();

	alrm.sa_handler = sigalrm;
	alrm.sa_flags = 0;
	sigaction (SIGALRM, &alrm, NULL);

	sint.sa_handler = sigint;
	sint.sa_flags = 0;
	sigaction (SIGINT, &sint, NULL);

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
	getmaxyx (stdscr, ci.rows, ci.cols);
	create_display_panel (ci.rows, ci.cols);
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

	return client_loop (fd);
}

int syntax (void)
{
	fprintf (stderr,
		 "Usage: nrvc <host>:<port>\n"
		 "       nrvc <tty>\n"
		 "       nrvc --help\n"
		 "       nrvc --version\n");
	return 0;
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
	remote_host = strdup (host);
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

int main (int argc, char *argv[])
{
	int fd;

	if (argc < 2) {
		syntax ();
		exit (1);
	}

	if (!strcmp (argv[1], "--version")) {
		printf ("ncurses-based RVC (vtgrab %s)\n", VERSION);
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
