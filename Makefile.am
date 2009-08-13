AUTOMAKE_OPTIONS = foreign
SUBDIRS = src docs

libdvbsubdocdir = ${prefix}/doc/libdvbsub
libdvbsubdoc_DATA = \
	README\
	COPYING\
	AUTHORS\
	ChangeLog\
	NEWS


pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libdvbsub-1.pc

$(pkgconfig_DATA): config.status

EXTRA_DIST = $(libdvbsubdoc_DATA)

MAINTAINERCLEANFILES = \
	$(srcdir)/aclocal.m4 \
	$(srcdir)/config.guess \
	$(srcdir)/config.h.in \
	$(srcdir)/config.sub \
	$(srcdir)/depcomp \
	$(srcdir)/install-sh \
	$(srcdir)/ltmain.sh \
	$(srcdir)/missing

DISTCHECK_CONFIGURE_FLAGS=--enable-gtk-doc

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

-include $(top_srcdir)/git.mk
