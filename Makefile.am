ACLOCAL_AMFLAGS = -I m4
NULL =

bin_PROGRAMS = usbclerk usbclerktest

usbclerk_LDFLAGS = $(USBCLERK_LIBS) -lversion -lsetupapi -lole32 -all-static -municode
usbclerk_CPPFLAGS = $(USBCLERK_CFLAGS)  -DUNICODE -D_UNICODE
usbclerk_SOURCES = usbclerk.cpp vdlog.cpp usbclerk.h vdlog.h

usbclerktest_LDFLAGS = -all-static -municode
usbclerktest_CPPFLAGS = -DUNICODE -D_UNICODE
usbclerktest_SOURCES = usbclerktest.cpp

EXTRA_DIST = usbclerk.wxs.in
CONFIG_STATUS_DEPENDENCIES = usbclerk.wxs.in

deps.txt:
	$(AM_V_GEN)rpm -qa | grep $(host_os) | sort | unix2dos > $@

MANUFACTURER = The Spice Project

usbclerk-$(WIXL_ARCH)-$(VERSION)$(BUILDID).msi: usbclerk.wxs deps.txt all
	$(AM_V_GEN)MANUFACTURER="$(MANUFACTURER)" wixl --arch $(WIXL_ARCH) -o $@ $<

msi: usbclerk-$(WIXL_ARCH)-$(VERSION)$(BUILDID).msi

CLEANFILES = usbclerk-$(WIXL_ARCH)-$(VERSION)$(BUILDID).msi

.PHONY: msi
