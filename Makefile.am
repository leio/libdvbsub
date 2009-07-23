AUTOMAKE_OPTIONS = foreign
SUBDIRS = src 

libdvbsubdocdir = ${prefix}/doc/libdvbsub
libdvbsubdoc_DATA = \
	README\
	COPYING\
	AUTHORS\
	ChangeLog\
	NEWS


EXTRA_DIST = $(libdvbsubdoc_DATA)


# Remove doc directory on uninstall
uninstall-local:
	-rm -r $(libdvbsubdocdir)

ChangeLog:
	git log >ChangeLog
