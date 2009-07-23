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
	@echo Creating $@
	@if test -d "$(srcdir)/.git"; then \
	  (GIT_DIR=$(top_srcdir)/.git ./missing --run git log --stat) | fmt --split-only > $@.tmp \
	  && mv -f $@.tmp $@ \
	  || ($(RM) $@.tmp; \
	      echo Failed to generate ChangeLog, your ChangeLog may be outdated >&2; \
	      (test -f $@ || echo git-log is required to generate this file >> $@)); \
	else \
	  test -f $@ || \
	  (echo A git checkout and git-log is required to generate ChangeLog >&2 && \
	  echo A git checkout and git-log is required to generate this file >> $@); \
	fi
