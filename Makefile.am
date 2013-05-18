ACLOCAL_AMFLAGS = -I m4
NULL =

bin_PROGRAMS = usbclerk usbclerktest

usbclerk_LDFLAGS = $(USBCLERK_LIBS) -lversion -lsetupapi -all-static
usbclerk_CPPFLAGS = $(USBCLERK_CFLAGS)
usbclerk_SOURCES = usbclerk.cpp vdlog.cpp usbclerk.h vdlog.h

usbclerktest_LDFLAGS =
usbclerktest_SOURCES = usbclerktest.cpp

EXTRA_DIST = usbclerk.wxs.in
CONFIG_STATUS_DEPENDENCIES = usbclerk.wxs.in
