bin_PROGRAMS = rvc rvcd
rvc_SOURCES = rvc.c
rvcd_SOURCES = rvcd.c
docdir=$(datadir)/doc/$(PACKAGE)
doc_DATA = doc/rvc
pkgdata_DATA = Makefile.am
CFLAGS = -Wall -W -O2

EXTRA_DIST = BUGS doc/rvc.sgml doc/README rvc.h

%: %.sgml
	@echo Use dbtohtml to make $@
