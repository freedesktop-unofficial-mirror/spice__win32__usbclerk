AC_INIT([usbclerk], [0.3.3],
        [spice-devel@lists.freedesktop.org])
AC_PREREQ([2.63])

AC_CONFIG_MACRO_DIR([m4])
AM_CONFIG_HEADER([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign dist-bzip2 -Wall -Werror])
AM_MAINTAINER_MODE([enable])
AC_CANONICAL_HOST

case "$host" in
    amd64*|x86_64*)
        WIXL_ARCH="x64"
    ;;
    *)
        WIXL_ARCH="x86"
    ;;
esac
AC_SUBST(WIXL_ARCH)

LT_INIT
AC_PROG_CXX

USBCLERK_DEPS="libwdi libusbredirparser-0.5 >= 0.6"
PKG_CHECK_MODULES(USBCLERK, $USBCLERK_DEPS)
LIBWDI_LIBS=`$PKG_CONFIG --static --libs $USBCLERK_DEPS`

AC_ARG_WITH([buildid],
  AS_HELP_STRING([--with-buildid=id], [Set additional build version details]))
AC_DEFINE_UNQUOTED([BUILDID], "$with_buildid", [Build version details])
if test "x$with_buildid" != x; then
  AC_SUBST([BUILDID], "-$with_buildid")
fi

major=`echo $PACKAGE_VERSION | cut -d. -f1`
minor=`echo $PACKAGE_VERSION | cut -d. -f2`
micro=`echo $PACKAGE_VERSION | cut -d. -f3`
buildid=`echo $with_buildid | cut -d. -f1`
if test "x$buildid" = x; then
  buildid=0
fi
build=`expr $micro \* 256 + $buildid`
WINDOWS_PRODUCTVERSION="$major.$minor.$build"
AC_SUBST([WINDOWS_PRODUCTVERSION])

m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AC_OUTPUT([
Makefile
usbclerk.wxs
])

AC_MSG_NOTICE([

        usbclerk $VERSION
        ==============

        c++ compiler:             ${CXX}

        Now type 'make' to build $PACKAGE

])
