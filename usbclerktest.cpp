#include <stdio.h>
#include <tchar.h>
#include "usbclerk.h"

int _tmain(int argc, TCHAR* argv[], TCHAR* envp[])
{
    HANDLE pipe;
    USBClerkDriverOp dev = {{USB_CLERK_MAGIC, USB_CLERK_VERSION, 0, sizeof(USBClerkDriverOp)}};
    USBClerkReply reply;
    DWORD pipe_mode;
    DWORD bytes = 0;
    bool do_remove = false;

    if (argc < 2 || swscanf_s(argv[argc - 1], L"%hx:%hx", &dev.vid, &dev.pid) < 2 ||
                               (argc == 3 && !(do_remove = !wcscmp(argv[1], L"/u")))) {
        printf("Usage: usbclerktest [/u] vid:pid\n/u - uninstall driver\nvid:pid in hex\n");
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
    if (do_remove) {
        printf("Removing %04x:%04x\n", dev.vid, dev.pid);
        dev.hdr.type = USB_CLERK_DRIVER_REMOVE;
    } else {
        printf("Signing & installing %04x:%04x\n", dev.vid, dev.pid);
        dev.hdr.type = USB_CLERK_DRIVER_INSTALL;
    }
    if (!TransactNamedPipe(pipe, &dev, sizeof(dev), &reply, sizeof(reply), &bytes, NULL)) {
        printf("TransactNamedPipe() failed: %d\n", GetLastError());
        CloseHandle(pipe);
        return 1;
    }
    CloseHandle(pipe);
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
    return 0;
}
