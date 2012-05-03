#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <tchar.h>
#include "usbclerk.h"
#include "libwdi.h"
#include "vdlog.h"

//#define DEBUG_USB_CLERK

#define USB_CLERK_DISPLAY_NAME      TEXT("USB Clerk")
#define USB_CLERK_NAME              TEXT("usbclerk")
#define USB_CLERK_DESCRIPTION       TEXT("Enables automathic winusb driver signing & install")
#define USB_CLERK_LOAD_ORDER_GROUP  TEXT("")
#define USB_CLERK_LOG_PATH          TEXT("%susbclerk.log")
#define USB_CLERK_PIPE_TIMEOUT      10000
#define USB_CLERK_PIPE_BUF_SIZE     1024
#define USB_DRIVER_PATH             "%Swdi_usb_driver"
#define USB_DRIVER_INFNAME_LEN      64
#define USB_DRIVER_INSTALL_RETRIES  7
#define USB_DRIVER_INSTALL_INTERVAL 1000

class USBClerk {
public:
    static USBClerk* get();
    ~USBClerk();
    bool run();
    bool install();
    bool uninstall();

private:
    USBClerk();
    bool execute();
    bool dispatch_message(CHAR *buffer, DWORD bytes, USBClerkReply *reply);
    bool install_winusb_driver(int vid, int pid);
    static DWORD WINAPI control_handler(DWORD control, DWORD event_type,
                                        LPVOID event_data, LPVOID context);
    static VOID WINAPI main(DWORD argc, TCHAR * argv[]);

private:
    static USBClerk* _singleton;
    SERVICE_STATUS _status;
    SERVICE_STATUS_HANDLE _status_handle;
    char _wdi_path[MAX_PATH];
    HANDLE _pipe;
    bool _running;
    VDLog* _log;
};

USBClerk* USBClerk::_singleton = NULL;

USBClerk* USBClerk::get()
{
    if (!_singleton) {
        _singleton = new USBClerk();
    }
    return (USBClerk*)_singleton;
}

USBClerk::USBClerk()
    : _running (false)
    , _status_handle (0)
    , _log (NULL)
{
    _singleton = this;
}

USBClerk::~USBClerk()
{
    delete _log;
}

bool USBClerk::run()
{
#ifndef DEBUG_USB_CLERK
    SERVICE_TABLE_ENTRY service_table[] = {{USB_CLERK_NAME, main}, {0, 0}};
    return !!StartServiceCtrlDispatcher(service_table);
#else
    main(0, NULL);
    return true;
#endif
}

bool USBClerk::install()
{
    bool ret = false;

    SC_HANDLE service_control_manager = OpenSCManager(0, 0, SC_MANAGER_CREATE_SERVICE);
    if (!service_control_manager) {
        printf("OpenSCManager failed\n");
        return false;
    }
    TCHAR path[_MAX_PATH + 1];
    if (!GetModuleFileName(0, path, sizeof(path) / sizeof(path[0]))) {
        printf("GetModuleFileName failed\n");
        CloseServiceHandle(service_control_manager);
        return false;
    }
    //FIXME: SERVICE_INTERACTIVE_PROCESS needed for xp only
    SC_HANDLE service = CreateService(service_control_manager, USB_CLERK_NAME,
                                      USB_CLERK_DISPLAY_NAME, SERVICE_ALL_ACCESS,
                                      SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS,
                                      SERVICE_AUTO_START, SERVICE_ERROR_IGNORE, path,
                                      USB_CLERK_LOAD_ORDER_GROUP, 0, 0, 0, 0);
    if (service) {
        SERVICE_DESCRIPTION descr;
        descr.lpDescription = USB_CLERK_DESCRIPTION;
        if (!ChangeServiceConfig2(service, SERVICE_CONFIG_DESCRIPTION, &descr)) {
            printf("ChangeServiceConfig2 failed\n");
        }
        CloseServiceHandle(service);
        printf("Service installed successfully\n");
        ret = true;
    } else if (GetLastError() == ERROR_SERVICE_EXISTS) {
        printf("Service already exists\n");
        ret = true;
    } else {
        printf("Service not installed successfully, error %d\n", GetLastError());
    }
    CloseServiceHandle(service_control_manager);
    return ret;
}

bool USBClerk::uninstall()
{
    bool ret = false;

    SC_HANDLE service_control_manager = OpenSCManager(0, 0, SC_MANAGER_CONNECT);
    if (!service_control_manager) {
        printf("OpenSCManager failed\n");
        return false;
    }
    SC_HANDLE service = OpenService(service_control_manager, USB_CLERK_NAME,
                                    SERVICE_QUERY_STATUS | DELETE);
    if (!service) {
        printf("OpenService failed\n");
        CloseServiceHandle(service_control_manager);
        return false;
    }
    SERVICE_STATUS status;
    if (!QueryServiceStatus(service, &status)) {
        printf("QueryServiceStatus failed\n");
    } else if (status.dwCurrentState != SERVICE_STOPPED) {
        printf("Service is still running\n");
    } else if (DeleteService(service)) {
        printf("Service removed successfully\n");
        ret = true;
    } else {
        switch (GetLastError()) {
        case ERROR_ACCESS_DENIED:
            printf("Access denied while trying to remove service\n");
            break;
        case ERROR_INVALID_HANDLE:
            printf("Handle invalid while trying to remove service\n");
            break;
        case ERROR_SERVICE_MARKED_FOR_DELETE:
            printf("Service already marked for deletion\n");
            break;
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(service_control_manager);
    return ret;
}

DWORD WINAPI USBClerk::control_handler(DWORD control, DWORD event_type, LPVOID event_data,
                                        LPVOID context)
{
    USBClerk* s = _singleton;
    DWORD ret = NO_ERROR;

    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN: {
        HANDLE pipe;
        s->_status.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(s->_status_handle, &s->_status);
        s->_running = false;
        pipe = CreateFile(USB_CLERK_PIPE_NAME, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
        }
        break;
    }
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(s->_status_handle, &s->_status);
        break;
    default:
        ret = ERROR_CALL_NOT_IMPLEMENTED;
    }
    return ret;
}

#define USBCLERK_ACCEPTED_CONTROLS \
    (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE)

VOID WINAPI USBClerk::main(DWORD argc, TCHAR* argv[])
{
    USBClerk* s = _singleton;

    SERVICE_STATUS* status;
    TCHAR log_path[MAX_PATH];
    TCHAR temp_path[MAX_PATH];

    if (GetTempPath(MAX_PATH, temp_path)) {
        sprintf_s(s->_wdi_path, MAX_PATH, USB_DRIVER_PATH, temp_path);
        swprintf_s(log_path, MAX_PATH, USB_CLERK_LOG_PATH, temp_path);
        s->_log = VDLog::get(log_path);
    }
    vd_printf("***Service started***");
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    status = &s->_status;
    status->dwServiceType = SERVICE_WIN32;
    status->dwCurrentState = SERVICE_STOPPED;
    status->dwControlsAccepted = 0;
    status->dwWin32ExitCode = NO_ERROR;
    status->dwServiceSpecificExitCode = NO_ERROR;
    status->dwCheckPoint = 0;
    status->dwWaitHint = 0;
    s->_status_handle = RegisterServiceCtrlHandlerEx(USB_CLERK_NAME, &USBClerk::control_handler,
                                                     NULL);
    if (!s->_status_handle) {
#ifndef DEBUG_USB_CLERK
        vd_printf("RegisterServiceCtrlHandler failed");
        return;
#endif // DEBUG_USB_CLERK
    }

    // service is starting
    status->dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(s->_status_handle, status);

    // service running
    status->dwControlsAccepted |= USBCLERK_ACCEPTED_CONTROLS;
    status->dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(s->_status_handle, status);

    s->_running = true;
    s->execute();

    // service was stopped
    status->dwCurrentState = SERVICE_STOP_PENDING;
    SetServiceStatus(s->_status_handle, status);

    // service is stopped
    status->dwControlsAccepted &= ~USBCLERK_ACCEPTED_CONTROLS;
    status->dwCurrentState = SERVICE_STOPPED;
#ifndef DEBUG_USB_CLERK
    SetServiceStatus(s->_status_handle, status);
#endif //DEBUG_USB_CLERK
}

bool USBClerk::execute()
{
    SECURITY_ATTRIBUTES sec_attr;
    SECURITY_DESCRIPTOR* sec_desr;
    USBClerkReply reply = {{USB_CLERK_MAGIC, USB_CLERK_VERSION,
        USB_CLERK_REPLY, sizeof(USBClerkReply)}};
    CHAR buffer[USB_CLERK_PIPE_BUF_SIZE];
    DWORD bytes;

#if 0
    /* Hack for wdi logging */
    if (wdi_register_logger((HWND)1, 1, 1000) != 0) {
        vd_printf("wdi_register_logger failed");
    }
#endif
    sec_desr = (SECURITY_DESCRIPTOR*)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    InitializeSecurityDescriptor(sec_desr, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(sec_desr, TRUE, (PACL)NULL, FALSE);
    sec_attr.nLength = sizeof(sec_attr);
    sec_attr.bInheritHandle = TRUE;
    sec_attr.lpSecurityDescriptor = sec_desr;
    _pipe = CreateNamedPipe(USB_CLERK_PIPE_NAME, PIPE_ACCESS_DUPLEX |
                            FILE_FLAG_FIRST_PIPE_INSTANCE,
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
                            USB_CLERK_PIPE_BUF_SIZE, USB_CLERK_PIPE_BUF_SIZE, 0, &sec_attr);
    if (_pipe == INVALID_HANDLE_VALUE) {
        vd_printf("CreatePipe() failed: %u", GetLastError());
        return false;
    }
    while (_running) {
        if (!ConnectNamedPipe(_pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            printf("ConnectNamedPipe() failed: %u", GetLastError());
            break;
        }
        if (!ReadFile(_pipe, &buffer, sizeof(buffer), &bytes, NULL)) {
            vd_printf("ReadFile() failed: %d\n", GetLastError());
            goto disconnect;
        }
        if (!dispatch_message(buffer, bytes, &reply)) {
            goto disconnect;
        }
        if (!WriteFile(_pipe, &reply, sizeof(reply), &bytes, NULL)) {
            vd_printf("WriteFile() failed: %d\n", GetLastError());
            goto disconnect;
        }
        FlushFileBuffers(_pipe);
disconnect:
        DisconnectNamedPipe(_pipe);
    }
    CloseHandle(_pipe);
    return true;
}

bool USBClerk::dispatch_message(CHAR *buffer, DWORD bytes, USBClerkReply *reply)
{
    USBClerkHeader *hdr = (USBClerkHeader *)buffer;
    USBClerkDriverOp *dev;

    if (hdr->magic != USB_CLERK_MAGIC) {
        vd_printf("Bad message received, magic %u", hdr->magic);
        return false;
    }
    if (hdr->size != sizeof(USBClerkDriverOp)) {
        vd_printf("Wrong mesage size %u type %u", hdr->size, hdr->type);
        return false;
    }
    dev = (USBClerkDriverOp *)buffer;
    switch (hdr->type) {
    case USB_CLERK_DRIVER_INSTALL:
        vd_printf("Installing winusb driver for %04x:%04x", dev->vid, dev->pid);
        if (reply->status = install_winusb_driver(dev->vid, dev->pid)) {
            vd_printf("winusb driver install succeed");
        } else {
            vd_printf("winusb driver install failed");
        }
        break;
    default:
        vd_printf("Unknown message received, type %u", hdr->type);
        return false;
    }
    return true;
}

bool USBClerk::install_winusb_driver(int vid, int pid)
{
    struct wdi_device_info *wdidev, *wdilist;
    struct wdi_options_create_list wdi_list_opts;
    struct wdi_options_prepare_driver wdi_prep_opts;
    struct wdi_options_install_driver wdi_inst_opts;
    char infname[USB_DRIVER_INFNAME_LEN];
    bool installed = false;
    bool found = false;
    int r;

    /* find wdi device that matches the libusb device */
    memset(&wdi_list_opts, 0, sizeof(wdi_list_opts));
    wdi_list_opts.list_all = 1;
    wdi_list_opts.list_hubs = 0;
    wdi_list_opts.trim_whitespaces = 1;
    r = wdi_create_list(&wdilist, &wdi_list_opts);
    if (r != WDI_SUCCESS) {
        vd_printf("Device %04x:%04x wdi_create_list() failed -- %s (%d)",
                  vid, pid, wdi_strerror(r), r);
        return false;
    }

    vd_printf("Looking for device vid:pid %04x:%04x", vid, pid);
    for (wdidev = wdilist; wdidev != NULL && !(found = wdidev->vid == vid && wdidev->pid == pid);
         wdidev = wdidev->next);
    if (!found) {
        vd_printf("Device %04x:%04x was not found", vid, pid);
        goto cleanup;
    }

    vd_printf("Device %04x:%04x found", vid, pid);

    /* if the driver is already installed -- nothing to do */
    if (wdidev->driver) {
        vd_printf("Currently installed driver is %s", wdidev->driver);
        if (strcmp(wdidev->driver, "WinUSB") == 0) {
            vd_printf("WinUSB driver is already installed");
            installed = true;
            goto cleanup;
        }
    }

    /* inf filename is built out of vid and pid */
    r = sprintf_s(infname, sizeof(infname), "usb_device_%04x_%04x.inf", vid, pid);
    if (r <= 0) {
        vd_printf("inf file naming failed (%d)", r);
        goto cleanup;
    }

    vd_printf("Installing driver for USB device: \"%s\" (%04x:%04x) inf: %s",
              wdidev->desc, vid, pid, infname);
    memset(&wdi_prep_opts, 0, sizeof(wdi_prep_opts));
    wdi_prep_opts.driver_type = WDI_WINUSB;
    r = wdi_prepare_driver(wdidev, _wdi_path, infname, &wdi_prep_opts);
    if (r != WDI_SUCCESS) {
        vd_printf("Device %04x:%04x driver prepare failed -- %s (%d)",
                  vid, pid, wdi_strerror(r), r);
        goto cleanup;
    }

    memset(&wdi_inst_opts, 0, sizeof(wdi_inst_opts));
    for (int t = 0; t < USB_DRIVER_INSTALL_RETRIES; t++) {
        r = wdi_install_driver(wdidev, _wdi_path, infname, &wdi_inst_opts);
        if (r == WDI_ERROR_PENDING_INSTALLATION) {
            vd_printf("Another driver is installing, retry in %d ms",
                      USB_DRIVER_INSTALL_INTERVAL);
            Sleep(USB_DRIVER_INSTALL_INTERVAL);
        } else {
            /* break on success or any error other than pending installation */
            break;
        }
    }

    if (!(installed = (r == WDI_SUCCESS))) {
        vd_printf("Device %04x:%04x driver install failed -- %s (%d)",
                  vid, pid, wdi_strerror(r), r);
    }

cleanup:
    wdi_destroy_list(wdilist);
    return installed;
}

int _tmain(int argc, TCHAR* argv[], TCHAR* envp[])
{
    bool success = false;

    USBClerk* usbclerk = USBClerk::get();
    if (argc > 1) {
        if (lstrcmpi(argv[1], TEXT("install")) == 0) {
            success = usbclerk->install();
        } else if (lstrcmpi(argv[1], TEXT("uninstall")) == 0) {
            success = usbclerk->uninstall();
        } else {
            printf("Use: USBClerk install / uninstall\n");
        }
    } else {
        success = usbclerk->run();
    }
    delete usbclerk;
    return (success ? 0 : -1);
}
