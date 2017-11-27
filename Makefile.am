bin_PROGRAMS = rvc rvcd twiglet
rvc_SOURCES = rvc.c
rvcd_SOURCES = rvcd.c xfree86.c
twiglet_SOURCES = nrvc.c simple_panel.c
twiglet_LDFLAGS = -lncurses
AM_CFLAGS = -D_GNU_SOURCE -Wall -W -O2

EXTRA_DIST = BUGS doc/rvc.sgml doc/README rvc.h xfree86.h simple_panel.h \
		TODO Makefile.am twiglet.1 doc/rvc vtgrab.spec

%: %.sgml
	xmlto -o $@ html $<

tag:
	cvs tag -c `echo V@VERSION@ | tr . _`
