#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <string.h>
#include <tchar.h>
#include <list>
#include "usbclerk.h"
#include "usbredirfilter.h"
#include "libwdi.h"
#include "vdlog.h"

//#define DEBUG_USB_CLERK

#define USB_CLERK_DISPLAY_NAME      TEXT("USB Clerk")
#define USB_CLERK_NAME              TEXT("usbclerk")
#define USB_CLERK_DESCRIPTION       TEXT("Enables automatic winusb driver signing & install")
#define USB_CLERK_LOAD_ORDER_GROUP  TEXT("")
#define USB_CLERK_LOG_PATH          TEXT("%susbclerk.log")
#define USB_CLERK_PIPE_TIMEOUT      10000
#define USB_CLERK_PIPE_BUF_SIZE     1024
#define USB_CLERK_PIPE_MAX_CLIENTS  32
#define USB_DRIVER_PATH             "%S\\wdi_usb_driver"
#define USB_DRIVER_INFNAME_LEN      64
#define USB_DRIVER_INSTALL_RETRIES  10
#define USB_DRIVER_INSTALL_INTERVAL 2000
#define MAX_DEVICE_PROP_LEN         256
#define MAX_DEVICE_HCID_LEN         1024
#define MAX_DEVICE_FILTER_LEN       1024

typedef struct USBDev {
    UINT16 vid;
    UINT16 pid;
    bool auto_remove;
} USBDev;

typedef std::list<USBDev> USBDevs;

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
    bool dispatch_message(CHAR *buffer, DWORD bytes, USBClerkReply *reply, USBDevs *devs);
    bool install_winusb_driver(int vid, int pid);
    bool remove_winusb_driver(int vid, int pid);
    bool uninstall_inf(HDEVINFO devs, PSP_DEVINFO_DATA dev_info);
    bool remove_dev(HDEVINFO devs, PSP_DEVINFO_DATA dev_info);
    bool rescan();
    bool get_dev_info(HDEVINFO devs, int vid, int pid, SP_DEVINFO_DATA *dev_info, bool *has_winusb);
    bool get_dev_props(HDEVINFO devs, SP_DEVINFO_DATA *dev_info,
                       uint8_t *cls, uint8_t *subcls, uint8_t *proto);
    bool get_dev_ifaces(HDEVINFO devs, int vid, int pid, int *iface_count,
                        uint8_t **cls, uint8_t **subcls, uint8_t **proto);
    bool dev_filter_check(int vid, int pid, bool *has_winusb);
    static DWORD WINAPI control_handler(DWORD control, DWORD event_type,
                                        LPVOID event_data, LPVOID context);
    static DWORD WINAPI pipe_thread(LPVOID param);
    static VOID WINAPI main(DWORD argc, TCHAR * argv[]);

private:
    static USBClerk* _singleton;
    SERVICE_STATUS _status;
    SERVICE_STATUS_HANDLE _status_handle;
    struct usbredirfilter_rule *_filter_rules;
    int _filter_count;
    char _wdi_path[MAX_PATH];
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
    : _status_handle (0)
    , _filter_rules (NULL)
    , _filter_count (0)
    , _running (false)
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
    SERVICE_TABLE_ENTRY service_table[] = {{(LPWSTR)USB_CLERK_NAME, main}, {0, 0}};
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
        descr.lpDescription = (LPWSTR)USB_CLERK_DESCRIPTION;
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
        printf("Service not installed successfully, error %ld\n", GetLastError());
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
    CHAR filter_str[MAX_DEVICE_FILTER_LEN];
    HANDLE pipe, thread;
    DWORD tid;
    HKEY hkey;
    LONG ret;

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

    /* Read filter rules from registry */
    ret = RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Software\\USBClerk", 0, KEY_READ, &hkey);
    if (ret == ERROR_SUCCESS) {
        DWORD size = sizeof(filter_str);
        ret = RegQueryValueExA(hkey, "filter_rules", NULL, NULL, (LPBYTE)filter_str, &size);
        if (ret == ERROR_SUCCESS) {
            vd_printf("Filter rules: %s", filter_str);
            ret = usbredirfilter_string_to_rules(filter_str, ",", "|",
                                                 &_filter_rules, &_filter_count);
            if (ret == 0) {
                vd_printf("Filter count: %d", _filter_count);
            } else {
                vd_printf("Failed parsing filter rules: %ld", ret);
            }
        }
        RegCloseKey(hkey);
    }
    while (_running) {
        pipe = CreateNamedPipe(USB_CLERK_PIPE_NAME, PIPE_ACCESS_DUPLEX,
                               PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                               USB_CLERK_PIPE_MAX_CLIENTS, USB_CLERK_PIPE_BUF_SIZE,
                               USB_CLERK_PIPE_BUF_SIZE, 0, &sec_attr);
        if (pipe == INVALID_HANDLE_VALUE) {
            vd_printf("CreatePipe() failed: %ld", GetLastError());
            break;
        }
        if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            vd_printf("ConnectNamedPipe() failed: %ld", GetLastError());
            CloseHandle(pipe);
            break;
        }
        thread = CreateThread(NULL, 0, pipe_thread, (LPVOID)pipe, 0, &tid);
        if (thread == NULL) {
            vd_printf("CreateThread() failed: %ld", GetLastError());
            break;
        }
        CloseHandle(thread);
    }
    free(_filter_rules);
    return true;
}

DWORD WINAPI USBClerk::pipe_thread(LPVOID param)
{
    USBClerkReply reply = {{USB_CLERK_MAGIC, USB_CLERK_VERSION,
        USB_CLERK_REPLY, sizeof(USBClerkReply)}};
    CHAR buffer[USB_CLERK_PIPE_BUF_SIZE];
    HANDLE pipe = (HANDLE)param;
    USBClerk* usbclerk = get();
    USBDevs devs;
    DWORD bytes;

    while (usbclerk->_running) {
        if (!ReadFile(pipe, &buffer, sizeof(buffer), &bytes, NULL) ||
            !usbclerk->dispatch_message(buffer, bytes, &reply, &devs) ||
            !WriteFile(pipe, &reply, sizeof(reply), &bytes, NULL)) {
            break;
        }
        FlushFileBuffers(pipe);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    for (USBDevs::iterator dev = devs.begin(); dev != devs.end(); dev++) {
        if (dev->auto_remove) {
            usbclerk->remove_winusb_driver(dev->vid, dev->pid);
        }
    }
    return 0;
}

bool USBClerk::dispatch_message(CHAR *buffer, DWORD bytes, USBClerkReply *reply, USBDevs *devs)
{
    USBClerkHeader *hdr = (USBClerkHeader *)buffer;
    USBClerkDriverOp *op;

    if (hdr->magic != USB_CLERK_MAGIC) {
        vd_printf("Bad message received, magic %d", hdr->magic);
        return false;
    }
    if (hdr->size != sizeof(USBClerkDriverOp)) {
        vd_printf("Wrong mesage size %u type %u", hdr->size, hdr->type);
        return false;
    }
    op = (USBClerkDriverOp *)buffer;
    switch (hdr->type) {
    case USB_CLERK_DRIVER_SESSION_INSTALL:
    case USB_CLERK_DRIVER_INSTALL:
        vd_printf("Installing winusb driver for %04x:%04x", op->vid, op->pid);
        reply->status = install_winusb_driver(op->vid, op->pid);
        if (reply->status) {
            USBDev dev = {op->vid, op->pid, hdr->type == USB_CLERK_DRIVER_SESSION_INSTALL};
            devs->push_back(dev);
        }
        break;
    case USB_CLERK_DRIVER_REMOVE:
        // FIXME: check device is not used by another client
        vd_printf("Removing winusb driver for %04x:%04x", op->vid, op->pid);
        reply->status = remove_winusb_driver(op->vid, op->pid);
        // remove device from list to prevent another driver removal in pipe disconnect
        for (USBDevs::iterator d = devs->begin(); d != devs->end(); d++) {
            if (d->vid == op->vid && d->pid == op->pid) {
                devs->erase(d);
                break;
            }
        }
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
    bool installed;
    bool found = false;
    int r;

    if (!dev_filter_check(vid, pid, &installed)) {
        return false;
    }
    if (installed) {
        vd_printf("WinUSB driver is already installed");
        return true;
    }

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
    bool installed;
    bool ret = false;

    devs = SetupDiGetClassDevs(NULL, L"USB", NULL, DIGCF_ALLCLASSES);
    if (devs == INVALID_HANDLE_VALUE) {
        vd_printf("SetupDiGetClassDevsEx failed: %ld", GetLastError());
        return false;
    }
    if (get_dev_info(devs, vid, pid, &dev_info, &installed)) {
        if (installed) {
            vd_printf("Removing %04x:%04x", vid, pid);
            if (uninstall_inf(devs, &dev_info)) {
                ret = remove_dev(devs, &dev_info);
            }
        } else {
            vd_printf("WinUSB driver is not installed");
        }
    }
    SetupDiDestroyDeviceInfoList(devs);
    ret = ret && rescan();
    return ret;
}

bool USBClerk::uninstall_inf(HDEVINFO devs, PSP_DEVINFO_DATA dev_info)
{
    SP_DRVINFO_DATA drv_info;
    SP_DRVINFO_DETAIL_DATA drv_info_detail;
    SP_DEVINSTALL_PARAMS install_params = {0};
    TCHAR *inf_filename;

    install_params.cbSize = sizeof(SP_DEVINSTALL_PARAMS);
    if (!SetupDiGetDeviceInstallParams(devs, dev_info, &install_params)) {
        vd_printf("Failed to get device install params: %ld", GetLastError());
        return false;
    }
    install_params.FlagsEx |= DI_FLAGSEX_INSTALLEDDRIVER;
    if (!SetupDiSetDeviceInstallParams(devs, dev_info, &install_params)) {
        vd_printf("Failed to set device install params: %ld", GetLastError());
        return false;
    }
    if (!SetupDiBuildDriverInfoList(devs, dev_info, SPDIT_CLASSDRIVER)) {
        vd_printf("Cannot build driver info list: %ld", GetLastError());
        return false;
    }
    drv_info.cbSize = sizeof(SP_DRVINFO_DATA);
    if (!SetupDiEnumDriverInfo(devs, dev_info, SPDIT_CLASSDRIVER, 0, &drv_info)) {
        vd_printf("Failed to enumerate driver info: %ld", GetLastError());
        return false;
    }
    drv_info_detail.cbSize = sizeof(drv_info_detail);
    if (!SetupDiGetDriverInfoDetail(devs, dev_info, &drv_info, &drv_info_detail,
            sizeof(drv_info_detail), NULL) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        vd_printf("Cannot get driver info detail: %ld", GetLastError());
        return false;
    }
    vd_printf("Uninstalling inf: %S", drv_info_detail.InfFileName);
    inf_filename = wcsrchr(drv_info_detail.InfFileName, '\\') + 1;
    if (!SetupUninstallOEMInf(inf_filename, SUOI_FORCEDELETE, NULL)) {
        vd_printf("Failed to uninstall inf: %ld", GetLastError());
        return false;
    }
    return true;
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
        vd_printf("Failed setting class remove params: %ld", GetLastError());
        return false;
    }
    if (!SetupDiCallClassInstaller(DIF_REMOVE, devs, dev_info)) {
        vd_printf("Class remove failed: %ld", GetLastError());
        return false;
    }
    return true;
}

bool USBClerk::rescan()
{
    DEVINST dev_root;

    if (CM_Locate_DevNode_Ex(&dev_root, NULL, CM_LOCATE_DEVNODE_NORMAL, NULL) != CR_SUCCESS) {
        vd_printf("Device node cannot be located: %ld", GetLastError());
        return false;
    }
    if (CM_Reenumerate_DevNode_Ex(dev_root, 0, NULL) != CR_SUCCESS) {
        vd_printf("Device node enumeration failed: %ld", GetLastError());
        return false;
    }
    return true;
}

bool USBClerk::get_dev_info(HDEVINFO devs, int vid, int pid, SP_DEVINFO_DATA *dev_info,
                            bool *has_winusb)
{
    TCHAR dev_prefix[MAX_DEVICE_ID_LEN];
    TCHAR dev_id[MAX_DEVICE_ID_LEN];
    TCHAR service_name[MAX_DEVICE_PROP_LEN];
    bool dev_found = false;

    _sntprintf(dev_prefix, MAX_DEVICE_ID_LEN, TEXT("USB\\VID_%04X&PID_%04X\\"), vid, pid);
    dev_info->cbSize = sizeof(*dev_info);
    for (DWORD dev_index = 0; SetupDiEnumDeviceInfo(devs, dev_index, dev_info); dev_index++) {
        if (SetupDiGetDeviceInstanceId(devs, dev_info, dev_id, MAX_DEVICE_ID_LEN, NULL) &&
                (dev_found = !!wcsstr(dev_id, dev_prefix))) {
            break;
        }
    }
    if (!dev_found) {
        vd_printf("Cannot find device info %04X:%04X", vid, pid);
        return false;
    }
    if (has_winusb == NULL) {
        return true;
    }
    if (!SetupDiGetDeviceRegistryProperty(devs, dev_info, SPDRP_SERVICE, NULL,
            (PBYTE)service_name, sizeof(service_name), NULL)) {
        vd_printf("Cannot get device service name %ld", GetLastError());
        *has_winusb = false;
        return true;
    }
    *has_winusb = !wcscmp(service_name, L"WinUSB");
    return true;
}

bool USBClerk::get_dev_props(HDEVINFO devs, SP_DEVINFO_DATA *dev_info,
                             uint8_t *cls, uint8_t *subcls, uint8_t *proto)
{
    TCHAR compat_ids[MAX_DEVICE_HCID_LEN];

    *cls = *subcls = *proto = 0;
    if (!SetupDiGetDeviceRegistryProperty(devs, dev_info, SPDRP_COMPATIBLEIDS, NULL,
            (PBYTE)compat_ids, sizeof(compat_ids), NULL)) {
        vd_printf("Cannot get compatible id %ld", GetLastError());
        return false;
    }
    if (swscanf(compat_ids, L"USB\\Class_%02hx&SubClass_%02hx&Prot_%02hx",
            cls, subcls, proto) != 3) {
        vd_printf("Cannot parse compatible id %S", compat_ids);
        return false;
    }
    return true;
}

/* cls, subcls & proto for the interfaces are allocated here, so caller is responsible to
   delete [] them after use */
bool USBClerk::get_dev_ifaces(HDEVINFO devs, int vid, int pid, int *iface_count,
                              uint8_t **cls, uint8_t **subcls, uint8_t **proto)
{
    TCHAR dev_prefix[MAX_DEVICE_ID_LEN];
    TCHAR dev_id[MAX_DEVICE_ID_LEN];
    SP_DEVINFO_DATA dev_info;
    DWORD dev_index;
    bool ret = true;
    int i = 0;

    _sntprintf(dev_prefix, MAX_DEVICE_ID_LEN, TEXT("USB\\VID_%04X&PID_%04X\\&MI_"), vid, pid);
    dev_info.cbSize = sizeof(dev_info);
    *iface_count = 0;
    /* count interfaces */
    for (dev_index = 0; SetupDiEnumDeviceInfo(devs, dev_index, &dev_info); dev_index++) {
        if (SetupDiGetDeviceInstanceId(devs, &dev_info, dev_id, MAX_DEVICE_ID_LEN, NULL) &&
                wcsstr(dev_id, dev_prefix)) {
            *iface_count += 1;
        }
    }
    if (!*iface_count) {
        *cls = *subcls = *proto = NULL;
        return true;
    }
    vd_printf("iface_count %d", *iface_count);
    *cls = new uint8_t[*iface_count];
    *subcls = new uint8_t[*iface_count];
    *proto = new uint8_t[*iface_count];
    /* get properties for each of the interfaces */
    for (dev_index = 0; SetupDiEnumDeviceInfo(devs, dev_index, &dev_info); dev_index++) {
        if (SetupDiGetDeviceInstanceId(devs, &dev_info, dev_id, MAX_DEVICE_ID_LEN, NULL) &&
                wcsstr(dev_id, dev_prefix) && ret && i < *iface_count) {
            ret = get_dev_props(devs, &dev_info, cls[i], subcls[i], proto[i]);
            i++;
        }
    }
    return ret;
}

/* returns true if the device exists and passed the filter rules (or no filters at all).
   has_winusb is true if winusb driver is installed on the device. */
bool USBClerk::dev_filter_check(int vid, int pid, bool *has_winusb)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    uint8_t dev_cls, dev_subcls, dev_proto;
    uint8_t *iface_cls, *iface_subcls, *iface_proto;
    int iface_count = 0;
    bool ret = false;

    devs = SetupDiGetClassDevs(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        vd_printf("SetupDiGetClassDevsEx failed: %ld", GetLastError());
        return false;
    }
    if (!get_dev_info(devs, vid, pid, &dev_info, has_winusb)) {
        goto cleanup;
    }
    if (!_filter_rules) {
        ret = true;
        goto cleanup;
    }
    if (!get_dev_props(devs, &dev_info, &dev_cls, &dev_subcls, &dev_proto) ||
        !get_dev_ifaces(devs, vid, pid, &iface_count, &iface_cls, &iface_subcls, &iface_proto)) {
        goto cleanup;
    }
    /* device_version_bcd is ignored, as it is unavailable via setup api.
       we can get it when device is opened with libusb, which is currently not the case. */
    if (usbredirfilter_check(_filter_rules, _filter_count, dev_cls, dev_subcls, dev_proto,
            iface_cls, iface_subcls, iface_proto, iface_count, vid, pid, 0, 0) == 0) {
        ret = true;
    } else {
        vd_printf("Device filter failed %04x:%04x", vid, pid);
    }
cleanup:
    if (iface_count > 0) {
         delete []iface_cls;
         delete []iface_subcls;
         delete []iface_proto;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return ret;
}

extern "C"
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
