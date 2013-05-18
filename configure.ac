AC_INIT([usbclerk], [0.0.1.1],
        [spice-devel@lists.freedesktop.org])
AC_PREREQ([2.64])

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
