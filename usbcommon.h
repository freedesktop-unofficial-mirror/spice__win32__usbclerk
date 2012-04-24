#ifndef _H_USBCOMMON
#define _H_USBCOMMON

#include <windows.h>

#define USB_CLERK_PIPE_NAME     TEXT("\\\\.\\pipe\\usbclerkpipe")
#define USB_CLERK_MAGIC         0xDADA
#define USB_CLERK_VERSION       0x0001

typedef struct USBClerkHeader {
    UINT16 magic;
    UINT16 version;
    UINT16 type;
    UINT16 size;
} USBClerkHeader;

enum {
    USB_CLERK_DEV_INFO = 1,
    USB_CLERK_ACK,
    USB_CLERK_END_MESSAGE,
};

typedef struct USBClerkDevInfo {
    USBClerkHeader hdr;
    UINT16 vid;
    UINT16 pid;
} USBClerkDevInfo;

typedef struct USBClerkAck {
    USBClerkHeader hdr;
    UINT16 ack;
} USBClerkAck;

#endif
