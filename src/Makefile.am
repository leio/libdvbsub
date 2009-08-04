AUTOMAKE_OPTIONS = foreign
AM_CPPFLAGS = \
	-DPACKAGE_SRC_DIR=\""$(srcdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	$(LIBDVBSUB_CFLAGS)

AM_CFLAGS =\
	 -Wall\
	 $(DEBUGFLAGS)

bin_PROGRAMS = libdvbsub

libdvbsub_SOURCES = \
	main.c     \
	dvb-sub.h     \
	dvb-sub.c     \
	ffmpeg-colorspace.h

libdvbsub_LDADD = $(LIBDVBSUB_LIBS)
