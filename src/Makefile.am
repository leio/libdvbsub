AUTOMAKE_OPTIONS = foreign
AM_CPPFLAGS = \
	-DPACKAGE_SRC_DIR=\""$(srcdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	$(LIBDVBSUB_CFLAGS)

AM_CFLAGS =\
	 -Wall\
	 -g

bin_PROGRAMS = libdvbsub

libdvbsub_SOURCES = \
	main.c

libdvbsub_LDADD = $(LIBDVBSUB_LIBS)
