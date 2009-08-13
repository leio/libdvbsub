AC_INIT(libdvbsub, 0.1)

AM_INIT_AUTOMAKE(AC_PACKAGE_NAME, AC_PACKAGE_VERSION)
AC_CONFIG_HEADERS([config.h])
AM_MAINTAINER_MODE

AC_ISC_POSIX
AC_PROG_CC
AM_PROG_CC_STDC
AC_HEADER_STDC

AM_PROG_LIBTOOL

PKG_CHECK_MODULES(LIBDVBSUB, [glib-2.0 gobject-2.0 gstreamer-0.10 gstreamer-base-0.10 >= 0.10.22])



##################################################
# Check for gtk-doc and declare library version
##################################################
GTK_DOC_CHECK([1.9])
AC_SUBST(LIBDVBSUB_VERSION, AC_PACKAGE_VERSION)

#######
# Debug
#######
AC_ARG_ENABLE(debug, AS_HELP_STRING([--enable-debug],[Enable debugging information]), ,enable_debug=no)
dnl Have we asked for debugging?
if test "$enable_debug" = yes ; then
    DEBUGFLAGS="-g -DDEBUG"
else
    DEBUGFLAGS=""
fi

AC_SUBST(DEBUGFLAGS)

AC_OUTPUT([
Makefile
src/Makefile
docs/Makefile
docs/reference/Makefile
docs/reference/version.xml
libdvbsub-1.pc
])
