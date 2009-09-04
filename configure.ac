AC_INIT(libdvbsub-1, 0.1)
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE(AC_PACKAGE_NAME, AC_PACKAGE_VERSION)
AC_CONFIG_HEADERS([config.h])
AM_MAINTAINER_MODE

AC_ISC_POSIX
AC_PROG_CC
AM_PROG_CC_STDC
AC_HEADER_STDC

AM_PROG_LIBTOOL

GLIB_REQUIRED=2.10
GST_BASE_REQUIRED=0.10.22
PKG_CHECK_MODULES(LIBDVBSUB, [glib-2.0 >= $GLIB_REQUIRED gobject-2.0 >= $GLIB_REQUIRED gstreamer-0.10 gstreamer-base-0.10 >= $GST_BASE_REQUIRED])



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
    DEBUGFLAGS="-DG_DISABLE_ASSERT"
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
