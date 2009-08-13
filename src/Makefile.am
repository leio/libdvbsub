AUTOMAKE_OPTIONS = foreign
AM_CPPFLAGS = \
	-DPACKAGE_SRC_DIR=\""$(srcdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	$(LIBDVBSUB_CFLAGS)

AM_CFLAGS =\
	 -Wall\
	 $(DEBUGFLAGS)

lib_LTLIBRARIES = libdvbsub-1.la

libdvbsub_1_la_SOURCES = \
	dvb-sub.c     \
	ffmpeg-colorspace.h

pkginclude_HEADERS = \
	dvb-sub.h

libdvbsub_1_la_LIBADD = $(LIBDVBSUB_LIBS)


bin_PROGRAMS = dvbsub-test

dvbsub_test_SOURCES = \
	main.c

dvbsub_test_LDADD = $(LIBDVBSUB_LIBS) libdvbsub-1.la

-include $(top_srcdir)/git.mk
