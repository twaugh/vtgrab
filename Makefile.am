bin_PROGRAMS = rvc rvcd nrvc
rvc_SOURCES = rvc.c
rvcd_SOURCES = rvcd.c xfree86.c
nrvc_SOURCES = nrvc.c
nrvc_LDFLAGS = -lpanel -lncurses
docdir=$(datadir)/doc/$(PACKAGE)
doc_DATA = doc/rvc
CFLAGS = -D_GNU_SOURCE -Wall -W -O2

EXTRA_DIST = BUGS doc/rvc.sgml doc/README rvc.h xfree86.h TODO Makefile.am

%: %.sgml
	@echo Use dbtohtml to make $@
