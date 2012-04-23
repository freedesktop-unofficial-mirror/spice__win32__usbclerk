#ifndef _H_USBCOMMON
#define _H_USBCOMMON

#include <windows.h>

#define USB_CLERK_PIPE_NAME TEXT("\\\\.\\pipe\\usbclerkpipe")

typedef struct USBDevInfo {
    WORD vid;
    WORD pid;
} USBDevInfo;

#endif
