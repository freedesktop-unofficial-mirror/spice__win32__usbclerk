#include <stdio.h>
#include <conio.h>
#include <tchar.h>
#include "usbclerk.h"

int _tmain(int argc, TCHAR* argv[], TCHAR* envp[])
{
    HANDLE pipe;
    USBClerkDriverOp dev = {{USB_CLERK_MAGIC, USB_CLERK_VERSION,
        USB_CLERK_DRIVER_INSTALL, sizeof(USBClerkDriverOp)}};
    USBClerkReply reply;
    DWORD pipe_mode;
    DWORD bytes = 0;
    bool err = false;
    int i, devs = 0;

    for (i = 1; i < argc && !err; i++) {
        if (lstrcmpi(argv[i], TEXT("/t")) == 0) {
            dev.hdr.type = USB_CLERK_DRIVER_SESSION_INSTALL;
        } else if (lstrcmpi(argv[i], TEXT("/u")) == 0) {
            dev.hdr.type = USB_CLERK_DRIVER_REMOVE;
        } else if (_stscanf(argv[i], TEXT("%hx:%hx"), &dev.vid, &dev.pid) == 2) {
            devs++;
        } else {
            err = true;
        }
    }
    if (argc < 2 || err || devs < argc - 2) {
        printf("Usage: usbclerktest [/t][/u] vid:pid [vid1:pid1...]\n"
               "default - install driver for device vid:pid (in hex)\n"
               "/t - temporary install until session terminated\n"
               "/u - uninstall driver\n");
        return 1;
    }
    pipe = CreateFile(USB_CLERK_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                      0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        _tprintf(TEXT("Cannot open pipe %s: %lu\n"), USB_CLERK_PIPE_NAME, GetLastError());
        return 1;
    }
    pipe_mode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
    if (!SetNamedPipeHandleState(pipe, &pipe_mode, NULL, NULL)) {
        printf("SetNamedPipeHandleState() failed: %lu\n", GetLastError());
        return 1;
    }

    for (i = 1; i < argc && !err; i++) {
        if (_stscanf(argv[i], TEXT("%hx:%hx"), &dev.vid, &dev.pid) < 2) continue;
        switch (dev.hdr.type) {
        case USB_CLERK_DRIVER_SESSION_INSTALL:
        case USB_CLERK_DRIVER_INSTALL:
            printf("Signing & installing %04x:%04x...", dev.vid, dev.pid);
            break;
        case USB_CLERK_DRIVER_REMOVE:
            printf("Removing %04x:%04x...", dev.vid, dev.pid);
            break;
        }
        if (!TransactNamedPipe(pipe, &dev, sizeof(dev), &reply, sizeof(reply), &bytes, NULL)) {
            printf("TransactNamedPipe() failed: %lu\n", GetLastError());
            CloseHandle(pipe);
            return 1;
        }
        if (reply.hdr.magic != USB_CLERK_MAGIC || reply.hdr.type != USB_CLERK_REPLY ||
                reply.hdr.size != sizeof(USBClerkReply)) {
            printf("Unknown message received, magic 0x%x type %u size %u\n",
                   reply.hdr.magic, reply.hdr.type, reply.hdr.size);
            return 1;
        }
        if (reply.status) {
            printf("Completed successfully\n");
        } else {
            printf("Failed\n");
        }
    }

    if (dev.hdr.type == USB_CLERK_DRIVER_SESSION_INSTALL) {
        printf("Hit any key to terminate session\n");
        _getch();
    }
    CloseHandle(pipe);
    return 0;
}
