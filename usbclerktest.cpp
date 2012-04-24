#include <stdio.h>
#include <tchar.h>
#include "usbclerk.h"

int _tmain(int argc, TCHAR* argv[], TCHAR* envp[])
{
    HANDLE pipe;
    USBClerkDevInfo dev = {{USB_CLERK_MAGIC, USB_CLERK_VERSION, 
        USB_CLERK_DEV_INFO, sizeof(USBClerkDevInfo)}};
    USBClerkAck ack;
    DWORD pipe_mode;
    DWORD bytes = 0;

    if (argc < 3 || !swscanf_s(argv[1], L"%hx", &dev.vid) ||
                    !swscanf_s(argv[2], L"%hx", &dev.pid)) {
        printf("Use: usbclerktest VID PID\n");
        return 1;
    }
    pipe = CreateFile(USB_CLERK_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                      0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        printf("Cannot open pipe %S: %d\n", USB_CLERK_PIPE_NAME, GetLastError());
        return 1;
    }
    pipe_mode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
    if (!SetNamedPipeHandleState(pipe, &pipe_mode, NULL, NULL)) {
        printf("SetNamedPipeHandleState() failed: %d\n", GetLastError());
        return 1;
    }
    printf("Signing & installing %04x:%04x\n", dev.vid, dev.pid);
    if (!TransactNamedPipe(pipe, &dev, sizeof(dev), &ack, sizeof(ack), &bytes, NULL)) {
        printf("TransactNamedPipe() failed: %d\n", GetLastError());
        CloseHandle(pipe);
        return 1;
    }
    CloseHandle(pipe);
    if (ack.hdr.magic != USB_CLERK_MAGIC || ack.hdr.type != USB_CLERK_ACK ||
            ack.hdr.size != sizeof(USBClerkAck)) {
        printf("Unknown message received, magic 0x%x type %u size %u",
               ack.hdr.magic, ack.hdr.type, ack.hdr.size);
        return 1;
    }
    if (ack.ack) {
        printf("winusb driver install succeed");
    } else {
        printf("winusb driver install failed");
    }
    return 0;
}
