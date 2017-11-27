/* The task is to locate the X server associated with a tty, figure
 * out which display it is, and grab a valid MIT cookie for it. */

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/tty.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int reapees = 0;

static struct consolemap {
	unsigned long console;
	unsigned long display;
	char *authority_file;
	pid_t controlling_pid;
	pid_t minion_pid;
	unsigned short port;
	struct consolemap *next;
} *consoles;

static size_t readzt (FILE *f, char *buf, size_t size)
{
	size_t i;
	for (i = 0; i < size; i++) {
		int ch = fgetc (f);
		if (ch == EOF)
			break;
		if ((buf[i] = ch) == '\0')
			break;
	}
	if (i < size)
		buf[i] = '\0';
	return i;
}

static void handle_sigchld (int s __attribute__ ((unused)))
{
	fprintf (stderr, "Got SIGCHLD\n");
	reapees++;
	return;
}

static void forget (struct consolemap *forget_me)
{
	struct consolemap *c, **prev = &consoles;
	for (c = *prev; c; prev = &(*prev)->next, c = c->next) {
		if (c == forget_me) {
			*prev = c->next;
			break;
		}
	}
}

int reap_child (void)
{
	pid_t dead;
	int status;
	struct consolemap *c, **prev = &consoles;
	dead = waitpid (0, &status, WNOHANG);
	if (dead == -1)
		return 1;
	fprintf (stderr, "Reaping %d\n", dead);
	for (c = *prev; c; prev = &(*prev)->next, c = c->next) {
		if (c->minion_pid == dead) {
			*prev = c->next;
			free (c);
			break;
		}
	}
	return 0;
}

static int grab_cookie (struct consolemap *cons)
{
	int status;
	pid_t extract, merge;
	int fildes[2];
	int null;
	char display[20];
	char *path, *oldpath;
	size_t pathlen, x11len;
	/* Get /usr/X11R6/bin into the path */
	const char *x11 = "/usr/X11R6/bin/:";
	oldpath = getenv ("PATH");
	pathlen = strlen (oldpath);
	x11len = strlen (x11);
	path = malloc (pathlen + x11len + 1);
	if (!path)
		return 1;
	memcpy (path, x11, x11len);
	memcpy (path + x11len, oldpath, pathlen);
	path[x11len + pathlen] = '\0';
	sprintf (display, ":%lu", cons->display);
	null = open ("/dev/null", O_RDWR);
	if (null == -1) {
		free (path);
		return 1;
	}
	if (pipe (fildes)) {
		free (path);
		return 1;
	}
	fflush (NULL);
	merge = fork ();
	if (merge == -1) {
		free (path);
		return 1;
	}
	if (merge == 0) {
		/* Child */
		alarm (0);
		close (STDIN_FILENO);
		close (STDOUT_FILENO);
		close (STDERR_FILENO);
		dup (fildes[0]); /* stdin */
		dup (null); /* stdout */
		dup (null); /* stderr */
		close (null);
		close (fildes[0]);
		close (fildes[1]);
		setenv ("PATH", path, 1);
		execlp ("xauth", "xauth", "merge", "-", NULL);
		exit (1);
	}
	close (fildes[0]);
	extract = fork ();
	if (extract == -1) {
		free (path);
		close (fildes[1]);
		close (null);
		wait (NULL);
		return 1;
	}
	if (extract == 0) {
		/* Child */
		alarm (0);
		close (STDIN_FILENO);
		close (STDOUT_FILENO);
		close (STDERR_FILENO);
		dup (null); /* stdin */
		dup (fildes[1]); /* stdout */
		dup (null); /* stderr */
		close (null);
		close (fildes[1]);
		setenv ("PATH", path, 1);
		execlp ("xauth", "xauth", "-f", cons->authority_file,
			"extract", "-", display, NULL);
		exit (1);
	}
	close (null);
	close (fildes[1]);
	waitpid (extract, &status, 0);
	if (WEXITSTATUS (status)) {
		wait (NULL);
		fprintf (stderr, "Extract failed with %d\n",
			 WEXITSTATUS (status));
		return 1;
	}
	waitpid (merge, &status, 0);
	if (WEXITSTATUS (status)) {
		fprintf (stderr, "Merge failed with %d\n",
			 WEXITSTATUS (status));
		return 1;
	}
	return 0;
}

static pid_t exec_minion (struct consolemap *cons)
{
	char display[20];
	pid_t minion;
	int fildes[2];
	int null;
	FILE *f;
	char *line;
	ssize_t linelen;

	sprintf (display, ":%lu", cons->display);
	null = open ("/dev/null", O_RDWR);
	if (null == -1)
		return -1;
	if (pipe (fildes)) {
		close (null);
		return -1;
	}
	minion = fork ();
	if (minion == -1) {
		close (null);
		return minion;
	}
	if (minion == 0) {
		/* Child */
		alarm (0);
		setenv ("DISPLAY", display, 1);
		close (STDIN_FILENO);
		close (STDOUT_FILENO);
		close (STDERR_FILENO);
		dup (null); /* stdin */
		dup (null); /* stdout */
		dup (fildes[1]); /* stderr */
		close (null);
		close (fildes[0]);
		close (fildes[1]);
		execlp ("x0rfbserver", "x0rfbserver", "--stealth", NULL);
		exit (1);
	}
	close (null);
	close (fildes[1]);
	cons->minion_pid = minion;

	cons->port = 5900;
	f = fdopen (fildes[0], "r");
	line = NULL;
	linelen = 0;
	getline (&line, &linelen, f);
	fclose (f);
	if (line && strstr (line, "port")) {
		char *p = line;
		char *end;
		unsigned long d;
		p += strcspn (p, "0123456789");
		d = strtoul (p, &end, 0);
		if (p != end)
			cons->port = d;
	}
	return minion;
}

static int use_display (struct consolemap *cons)
{
	grab_cookie (cons);
	cons->minion_pid = exec_minion (cons);
	return cons->minion_pid == -1 ? 1 : 0;
}

static int already_haunted (pid_t pid)
{
	struct consolemap *c;
	for (c = consoles; c; c = c->next)
		if (c->controlling_pid == pid)
			return 1;
	return 0;
}

static int create_minion (struct consolemap *cons)
{
	DIR *d;
	struct dirent *dent;
	int failed = 1;

	cons->minion_pid = -1;
	d = opendir ("/proc");
	if (!d) {
		perror ("/proc");
		return failed;
	}

	for (dent = readdir (d); dent; dent = readdir (d)) {
		char cmdline[PATH_MAX];
		char program[PATH_MAX];
		char arg[PATH_MAX];
		char *authority_file = NULL;
		unsigned long display = 0;
		unsigned long controlling_pid;
		char *p;
		FILE *fcmdline;

		if (!isdigit (dent->d_name[0]))
			continue;

		if (strlen (dent->d_name) + sizeof ("/proc/cmdline") >=
		    PATH_MAX)
			continue;

		sprintf (cmdline, "/proc/%s/cmdline", dent->d_name);
		fcmdline = fopen (cmdline, "r");
		if (!fcmdline)
			continue;

		if (readzt (fcmdline, program, sizeof (program)) ==
		    sizeof (program)) {
			fclose (fcmdline);
			continue;
		}

		p = strrchr (program, '/');
		if (p)
			p++;
		else p = program;

		if (strcmp (p, "X") && strncmp (p, "XF86", 4)) {
			fclose (fcmdline);
			continue;
		}

		controlling_pid = strtoul (dent->d_name, &p, 10);
		if (dent->d_name == p) {
			fclose (fcmdline);
			continue;
		}

		if (already_haunted (controlling_pid)) {
			fclose (fcmdline);
			continue;
		}

		/* Hunt for -auth and a display number */
		while (!feof (fcmdline)) {
			if (readzt (fcmdline, arg, sizeof (arg)) ==
			    sizeof (arg))
				continue;
			if (!authority_file && !strcmp (arg, "-auth")) {
				if (readzt (fcmdline, arg, sizeof (arg)) ==
				    sizeof (arg))
					continue;
				authority_file = strdup (arg);
			} else if (!display && arg[0] == ':') {
				unsigned long d;
				char *end;
				d = strtoul (arg + 1, &end, 10);
				if (arg + 1 == end)
					continue;
				display = d;
			}
		}

		if (!authority_file) {
			/* Use ~/.Xauthority */
			struct stat st;
			struct passwd *pw;
			size_t len;
			if (fstat (fileno (fcmdline), &st)) {
				fclose (fcmdline);
				continue;
			}
			pw = getpwuid (st.st_uid);
			if (!pw) {
				fclose (fcmdline);
				continue;
			}
			len = sizeof ("/.Xauthority") + 1;
			len += strlen (pw->pw_dir);
			authority_file = malloc (len);
			if (!authority_file) {
				fclose (fcmdline);
				continue;
			}
			sprintf (authority_file, "%s/.Xauthority", pw->pw_dir);
		}

		fclose (fcmdline);
		cons->controlling_pid = controlling_pid;
		cons->display = display;
		if (cons->authority_file)
			free (cons->authority_file);
		cons->authority_file = authority_file;
		if (!use_display (cons)) {
			failed = 0;
			break;
		}
	}

	closedir (d);
	return failed;
}

static int is_child (pid_t pid)
{
	int status = 0;
	int wpid;

	if (pid == -1)
		return 0;
	wpid = waitpid (pid, &status, WNOHANG);
	if (wpid == pid)
		return 0;
	if (wpid == -1)
		return 0;
	return 1;
}

static int do_switch (struct consolemap *c, unsigned long console)
{
	if (c && is_child (c->minion_pid))
		/* We already know about this one. */
		return 0;

	if (!c) {
		c = malloc (sizeof (struct consolemap));
		if (!c)
			return 0;
		c->console = console;
		c->authority_file = NULL;
	} else
		forget (c);

	if (!create_minion (c)) {
		c->next = consoles;
		consoles = c;
	} else
		free (c);
	return 0;
}

int dump_debug (void)
{
	struct consolemap *c;
	for (c = consoles; c; c = c->next) {
		printf ("\nConsole %lu:\n", c->console);
		printf ("%d is :%lu, served by %d on port %d\n",
			c->controlling_pid, c->display,
			c->minion_pid, c->port);
	}
	return 0;
}

int vt_switched_to (unsigned long console)
{
	struct consolemap *c;
	for (c = consoles; c; c = c->next) {
		if (c->console == console)
			break;
	}

	return do_switch (c, console);
}

int do_respawn (void)
{
	struct consolemap *c;
	for (c = consoles; c; c = c->next)
		do_switch (c, c->console);
	return 0;
}

int xfree86_init (void)
{
	struct sigaction chld;
	chld.sa_handler = handle_sigchld;
	sigemptyset (&chld.sa_mask);
	chld.sa_flags = SA_NOCLDSTOP | SA_RESTART;
	sigaction (SIGCHLD, &chld, NULL);
	return 0;
}

unsigned short port_for_console (unsigned long console)
{
	struct consolemap *c;
	for (c = consoles; c; c = c->next) {
		if (c->console == console && is_child (c->minion_pid))
			return c->port;
	}

	return 0;
}
