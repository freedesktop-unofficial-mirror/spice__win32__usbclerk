#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
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
#define USB_DRIVER_PATH             "%S\\wdi_usb_driver"
#define USB_DRIVER_INFNAME_LEN      64
#define USB_DRIVER_INSTALL_RETRIES  10
#define USB_DRIVER_INSTALL_INTERVAL 2000
#define MAX_DEVICE_PROP_LEN         256

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
    bool remove_winusb_driver(int vid, int pid);
    bool remove_dev(HDEVINFO devs, PSP_DEVINFO_DATA dev_info);
    bool rescan();
    bool get_dev_info(HDEVINFO devs, int vid, int pid, SP_DEVINFO_DATA *dev_info);
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
    TCHAR path[MAX_PATH];

    if (GetTempPath(MAX_PATH, path)) {
        swprintf_s(log_path, MAX_PATH, USB_CLERK_LOG_PATH, path);
        s->_log = VDLog::get(log_path);
    }
    if (GetSystemDirectory(path, MAX_PATH)) {
        sprintf_s(s->_wdi_path, MAX_PATH, USB_DRIVER_PATH, path);
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
            vd_printf("ConnectNamedPipe() failed: %u", GetLastError());
            break;
        }
        if (!ReadFile(_pipe, &buffer, sizeof(buffer), &bytes, NULL)) {
            vd_printf("ReadFile() failed: %d", GetLastError());
            goto disconnect;
        }
        if (!dispatch_message(buffer, bytes, &reply)) {
            goto disconnect;
        }
        if (!WriteFile(_pipe, &reply, sizeof(reply), &bytes, NULL)) {
            vd_printf("WriteFile() failed: %d", GetLastError());
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
        reply->status = install_winusb_driver(dev->vid, dev->pid);
        break;
    case USB_CLERK_DRIVER_REMOVE:
        vd_printf("Removing winusb driver for %04x:%04x", dev->vid, dev->pid);
        reply->status = remove_winusb_driver(dev->vid, dev->pid);        
        break;
    default:
        vd_printf("Unknown message received, type %u", hdr->type);
        return false;
    }
    if (reply->status) {
        vd_printf("Completed successfully");
    } else {
        vd_printf("Failed");
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
            if (t == 0) {
                vd_printf("Another driver is installing, will retry every %dms, up to %d times",
                          USB_DRIVER_INSTALL_INTERVAL, USB_DRIVER_INSTALL_RETRIES);
            }
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

bool USBClerk::remove_winusb_driver(int vid, int pid)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    bool ret = false;

    devs = SetupDiGetClassDevs(NULL, L"USB", NULL, DIGCF_ALLCLASSES);
    if (devs == INVALID_HANDLE_VALUE) {
        vd_printf("SetupDiGetClassDevsEx failed: %u", GetLastError());
        return false;
    }
    if (get_dev_info(devs, vid, pid, &dev_info)) {
        vd_printf("Removing %04x:%04x", vid, pid);
        ret = remove_dev(devs, &dev_info);
    }
    SetupDiDestroyDeviceInfoList(devs);
    ret = ret && rescan();
    return ret;
}

bool USBClerk::remove_dev(HDEVINFO devs, PSP_DEVINFO_DATA dev_info)
{
    SP_REMOVEDEVICE_PARAMS rmd_params;

    rmd_params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    rmd_params.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    rmd_params.Scope = DI_REMOVEDEVICE_GLOBAL;
    rmd_params.HwProfile = 0;
    if (!SetupDiSetClassInstallParams(devs, dev_info,
            &rmd_params.ClassInstallHeader, sizeof(rmd_params))) {
        vd_printf("Failed setting class remove params: %u", GetLastError());
        return false;
    }
    if (!SetupDiCallClassInstaller(DIF_REMOVE, devs, dev_info)) {
        vd_printf("Class remove failed: %u", GetLastError());
        return false;
    }
    return true;
}

bool USBClerk::rescan()
{
    DEVINST dev_root;

    if (CM_Locate_DevNode_Ex(&dev_root, NULL, CM_LOCATE_DEVNODE_NORMAL, NULL) != CR_SUCCESS) {
        vd_printf("Device node cannot be located: %u", GetLastError());
        return false;
    }
    if (CM_Reenumerate_DevNode_Ex(dev_root, 0, NULL) != CR_SUCCESS) {
        vd_printf("Device node enumeration failed: %u", GetLastError());
        return false;
    }
    return true;
}

bool USBClerk::get_dev_info(HDEVINFO devs, int vid, int pid, SP_DEVINFO_DATA *dev_info)
{
    TCHAR dev_prefix[MAX_DEVICE_ID_LEN];
    TCHAR dev_id[MAX_DEVICE_ID_LEN];

    swprintf(dev_prefix, MAX_DEVICE_ID_LEN, L"USB\\VID_%04x&PID_%04x", vid, pid);
    dev_info->cbSize = sizeof(*dev_info);
    for (DWORD dev_index = 0; SetupDiEnumDeviceInfo(devs, dev_index, dev_info); dev_index++) {
        if (SetupDiGetDeviceInstanceId(devs, dev_info, dev_id, MAX_DEVICE_ID_LEN, NULL) &&
                wcsstr(dev_id, dev_prefix)) {
            return true;
        }
    }
    return false;
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
