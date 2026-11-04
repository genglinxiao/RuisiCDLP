#include "usb_dlp/windows_device.h"

#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <setupapi.h>

#include <cstdlib>
#include <cwchar>
#include <vector>

#ifdef USB_DLP_XP_FILTER
#include "usb_dlp/xp_filter_protocol.h"
#endif

namespace usb_dlp {
namespace {

bool locate(const std::wstring& instance_id, DEVINST* node, CONFIGRET* result) {
  std::vector<wchar_t> mutable_id(instance_id.begin(), instance_id.end());
  mutable_id.push_back(L'\0');
  *result = CM_Locate_DevNodeW(node, &mutable_id[0], CM_LOCATE_DEVNODE_NORMAL);
  return *result == CR_SUCCESS;
}

#ifdef USB_DLP_XP_FILTER
bool uses_filter(const std::wstring& instance_id) {
  return instance_id.size() > 8 &&
      _wcsnicmp(instance_id.c_str(), L"USBSTOR\\", 8) == 0;
}

bool physical_name(const std::wstring& instance_id, wchar_t* name,
                   unsigned long* result_code, std::wstring* diagnostic) {
  DEVINST node = 0;
  CONFIGRET result = CR_FAILURE;
  if (!locate(instance_id, &node, &result)) {
    if (result_code != nullptr) *result_code = result;
    if (diagnostic != nullptr) *diagnostic = L"locate_devnode";
    return false;
  }
  ULONG bytes = sizeof(wchar_t) * USB_DLP_FILTER_NAME_CAPACITY;
  result = CM_Get_DevNode_Registry_PropertyW(
      node, CM_DRP_PHYSICAL_DEVICE_OBJECT_NAME, nullptr, name, &bytes, 0);
  if (result_code != nullptr) *result_code = result;
  if (diagnostic != nullptr) *diagnostic = L"read_pdo_name";
  if (result != CR_SUCCESS || bytes < sizeof(wchar_t) ||
      bytes > sizeof(wchar_t) * USB_DLP_FILTER_NAME_CAPACITY) return false;
  name[USB_DLP_FILTER_NAME_CAPACITY - 1] = L'\0';
  if (name[0] == L'\0') return false;
  return true;
}

bool filter_request(const std::wstring& instance_id, bool set_allowed,
                    bool allow, bool* found, bool* actual_allowed,
                    unsigned long* result_code, unsigned long* attached_count,
                    std::wstring* diagnostic) {
  USB_DLP_FILTER_REQUEST request = {};
  request.version = USB_DLP_FILTER_PROTOCOL_VERSION;
  request.allowed = allow ? 1 : 0;
  if (!physical_name(instance_id, request.physical_device_name, result_code, diagnostic))
    return false;
  HANDLE handle = CreateFileW(L"\\\\.\\RuisiUsbDlpFilter", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (result_code != nullptr) *result_code = GetLastError();
    if (diagnostic != nullptr) *diagnostic = L"open_filter_control";
    return false;
  }
  USB_DLP_FILTER_REPLY reply = {};
  DWORD returned = 0;
  const BOOL success = DeviceIoControl(
      handle, set_allowed ? IOCTL_USB_DLP_FILTER_SET_ALLOWED : IOCTL_USB_DLP_FILTER_QUERY,
      &request, sizeof(request), &reply, sizeof(reply), &returned, nullptr);
  const DWORD result = success ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (result_code != nullptr) *result_code = result;
  if (diagnostic != nullptr) *diagnostic = L"filter_ioctl";
  if (!success || returned != sizeof(reply) ||
      reply.version != USB_DLP_FILTER_PROTOCOL_VERSION) {
    if (success && result_code != nullptr) *result_code = ERROR_INVALID_DATA;
    return false;
  }
  if (found != nullptr) *found = reply.found != 0;
  if (actual_allowed != nullptr) *actual_allowed = reply.allowed != 0;
  if (attached_count != nullptr) *attached_count = reply.attached_usb_disks;
  if (diagnostic != nullptr) *diagnostic = reply.found ? L"attached" : L"device_not_attached";
  return reply.found != 0;
}
#endif

// Class-installer state change. This is what Device Manager itself uses and it
// writes the state into the device's config flags, so it survives the
// re-enumeration that follows a USB re-plug on XP.
bool change_state(const std::wstring& instance_id, DWORD state, unsigned long* result_code) {
  HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
  if (set == INVALID_HANDLE_VALUE) {
    if (result_code != nullptr) *result_code = GetLastError();
    return false;
  }
  SP_DEVINFO_DATA info = {};
  info.cbSize = sizeof(info);
  bool changed = false;
  DWORD result = ERROR_SUCCESS;
  if (!SetupDiOpenDeviceInfoW(set, instance_id.c_str(), nullptr, 0, &info)) {
    result = GetLastError();
  } else {
    SP_DEVINSTALL_PARAMS_W install = {};
    install.cbSize = sizeof(install);
    if (!SetupDiGetDeviceInstallParamsW(set, &info, &install)) {
      result = GetLastError();
    } else {
      install.Flags |= DI_QUIETINSTALL;
      if (!SetupDiSetDeviceInstallParamsW(set, &info, &install)) {
        result = GetLastError();
      } else {
        SP_PROPCHANGE_PARAMS params = {};
        params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        params.StateChange = state;
        params.Scope = DICS_FLAG_GLOBAL;
        if (!SetupDiSetClassInstallParamsW(
                set, &info, &params.ClassInstallHeader, sizeof(params))) {
          result = GetLastError();
        } else if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info)) {
          result = GetLastError();
        } else {
          changed = true;
          if (SetupDiGetDeviceInstallParamsW(set, &info, &install) &&
              (install.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0) {
            result = ERROR_SUCCESS_REBOOT_REQUIRED;
          }
        }
      }
    }
  }
  SetupDiDestroyDeviceInfoList(set);
  if (result_code != nullptr) *result_code = result;
  return changed;
}

#ifndef USB_DLP_XP_SETUPAPI
// XP quirk observed on real hardware: right after a re-plug the USB function
// device re-enumerates its storage child, so CM_Disable_DevNode() on the disk
// node returns CR_SUCCESS and the child is enabled again a moment later. The
// first insert sticks, every later insert does not. Disabling the parent
// function device (the node whose service is USBSTOR) is what makes it hold.
// The service check is mandatory: never disable a hub.
bool storage_function_parent(DEVINST node, DEVINST* parent) {
  DEVINST candidate = 0;
  if (CM_Get_Parent(&candidate, node, 0) != CR_SUCCESS) return false;
  wchar_t service[64] = {};
  ULONG size = sizeof(service);
  ULONG type = 0;
  if (CM_Get_DevNode_Registry_PropertyW(candidate, CM_DRP_SERVICE, &type, service,
                                        &size, 0) != CR_SUCCESS) return false;
  if (_wcsicmp(service, L"USBSTOR") != 0) return false;
  *parent = candidate;
  return true;
}

bool node_is_disabled(DEVINST node, unsigned long* status_out, unsigned long* problem_out) {
  ULONG status = 0;
  ULONG problem = 0;
  if (CM_Get_DevNode_Status(&status, &problem, node, 0) != CR_SUCCESS) return false;
  if (status_out != nullptr) *status_out = status;
  if (problem_out != nullptr) *problem_out = problem;
  return (status & DN_HAS_PROBLEM) != 0 && problem == CM_PROB_DISABLED;
}

// Extracts N from the PDO name "\Device\Harddisk2\DR7".
bool disk_number_of(const std::wstring& instance_id, ULONG* number) {
  DEVINST node = 0;
  CONFIGRET result = CR_FAILURE;
  if (!locate(instance_id, &node, &result)) return false;
  wchar_t pdo_name[256] = {};
  ULONG size = sizeof(pdo_name);
  ULONG type = 0;
  if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_PHYSICAL_DEVICE_OBJECT_NAME, &type,
                                        pdo_name, &size, 0) != CR_SUCCESS) return false;
  const wchar_t* marker = wcsstr(pdo_name, L"Harddisk");
  if (marker == nullptr) return false;
  const wchar_t* digits = marker + 8;
  if (*digits < L'0' || *digits > L'9') return false;
  *number = static_cast<ULONG>(_wtoi(digits));
  return true;
}

std::wstring windows_drive_letter() {
  wchar_t root[MAX_PATH] = {};
  if (GetWindowsDirectoryW(root, MAX_PATH) == 0) return std::wstring();
  return std::wstring(1, root[0]);
}

// Last resort for devices whose devnode refuses to stay disabled - typically
// 2 TB USB disks with a GPT layout that XP keeps re-probing. The driver stack
// stays alive, but every volume on that disk is locked, dismounted and its
// drive letter deleted, so the user cannot reach the data. Requires the process
// to keep running (the next poll repeats it after a re-enumeration).
bool dismount_disk_volumes(ULONG disk_number, int* dismounted) {
  bool touched = false;
  const std::wstring system_letter = windows_drive_letter();
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    const std::wstring drive(1, letter);
    if (!system_letter.empty() && _wcsicmp(drive.c_str(), system_letter.c_str()) == 0) continue;
    const std::wstring device_path = std::wstring(L"\\\\.\\") + letter + L":";
    HANDLE volume = CreateFileW(device_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) continue;
    VOLUME_DISK_EXTENTS extents = {};
    DWORD returned = 0;
    const BOOL queried = DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr,
                                         0, &extents, sizeof(extents), &returned, nullptr);
    if (!queried || extents.NumberOfDiskExtents == 0 ||
        extents.Extents[0].DiskNumber != disk_number) {
      CloseHandle(volume);
      continue;
    }
    DeviceIoControl(volume, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr);
    DeviceIoControl(volume, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr);
    CloseHandle(volume);
    // Drive letters live in the DOS device namespace; removing the symbolic link
    // here is what actually hides the volume from Explorer on XP.
    const std::wstring target = drive + L":";
    if (DefineDosDeviceW(DDD_REMOVE_DEFINITION, target.c_str(), nullptr)) touched = true;
    if (dismounted != nullptr) ++(*dismounted);
  }
  return touched;
}
#endif

}  // namespace

bool WindowsDeviceEnforcer::disable(
    const std::wstring& instance_id,
    unsigned long* result_code,
    std::wstring* diagnostic) const {
#ifdef USB_DLP_XP_FILTER
  if (uses_filter(instance_id)) {
    bool found = false;
    bool allowed = true;
    const bool success = filter_request(instance_id, true, false, &found,
                                        &allowed, result_code, nullptr, diagnostic);
    if (success && !allowed) return true;
    if (result_code != nullptr && *result_code == ERROR_SUCCESS)
      *result_code = ERROR_NOT_FOUND;
    return false;
  }
#endif
  (void)diagnostic;
#ifdef USB_DLP_XP_SETUPAPI
  unsigned long result = 0;
  const bool changed = change_state(instance_id, DICS_DISABLE, &result);
  bool disabled = false;
  unsigned long status_result = 0;
  const bool verified = changed && is_disabled(instance_id, &disabled, &status_result) && disabled;
  if (result_code != nullptr) {
    *result_code = verified ? ERROR_SUCCESS :
        (result != ERROR_SUCCESS ? result : ERROR_GEN_FAILURE);
  }
  return verified;
#else
  DEVINST node = 0;
  CONFIGRET result = CR_FAILURE;
  if (!locate(instance_id, &node, &result)) {
    if (result_code != nullptr) *result_code = result;
    return false;
  }
  result = CM_Disable_DevNode(node, 0);
  if (result == CR_SUCCESS) {
    DEVINST parent = 0;
    if (storage_function_parent(node, &parent)) CM_Disable_DevNode(parent, 0);
  }
  ULONG status = 0;
  ULONG problem = 0;
  if (!node_is_disabled(node, &status, &problem)) {
    // XP: CM_Disable_DevNode can report CR_SUCCESS while the USB stack
    // re-enumerates the disk and it comes back enabled. Fall back to the class
    // installer path with a global state change, which persists.
    unsigned long setup_result = 0;
    if (change_state(instance_id, DICS_DISABLE, &setup_result)) result = CR_SUCCESS;
  }
  if (!node_is_disabled(node, &status, &problem)) {
    // Still alive: large USB disks with an unreadable partition table keep being
    // re-probed. Take the volumes away instead of fighting the driver stack.
    ULONG disk = 0;
    int dismounted = 0;
    if (disk_number_of(instance_id, &disk)) dismount_disk_volumes(disk, &dismounted);
    if (dismounted > 0) {
      // The pending removal was almost certainly parked on an open volume handle
      // (DN_WILL_BE_REMOVED). Now that the volumes are gone the PnP manager can
      // finish, so retry once here instead of waiting for the next poll - that
      // wait is what keeps an already plugged disk readable for up to a minute
      // after its approval expires.
      CM_Disable_DevNode(node, 0);
      DEVINST parent = 0;
      if (storage_function_parent(node, &parent)) CM_Disable_DevNode(parent, 0);
    }
  }
  if (result_code != nullptr) *result_code = result;
  return result == CR_SUCCESS;
#endif
}

bool WindowsDeviceEnforcer::enable(
    const std::wstring& instance_id,
    unsigned long* result_code,
    std::wstring* diagnostic) const {
#ifdef USB_DLP_XP_FILTER
  if (uses_filter(instance_id)) {
    bool found = false;
    bool allowed = false;
    const bool success = filter_request(instance_id, true, true, &found,
                                        &allowed, result_code, nullptr, diagnostic);
    if (success && allowed) return true;
    if (result_code != nullptr && *result_code == ERROR_SUCCESS)
      *result_code = ERROR_NOT_FOUND;
    return false;
  }
#endif
  (void)diagnostic;
#ifdef USB_DLP_XP_SETUPAPI
  unsigned long result = 0;
  const bool changed = change_state(instance_id, DICS_ENABLE, &result);
  bool disabled = true;
  unsigned long status_result = 0;
  const bool verified = changed && is_disabled(instance_id, &disabled, &status_result) && !disabled;
  if (result_code != nullptr) {
    *result_code = verified ? ERROR_SUCCESS :
        (result != ERROR_SUCCESS ? result : ERROR_GEN_FAILURE);
  }
  return verified;
#else
  DEVINST node = 0;
  CONFIGRET result = CR_FAILURE;
  if (!locate(instance_id, &node, &result)) {
    if (result_code != nullptr) *result_code = result;
    return false;
  }
  DEVINST parent = 0;
  if (storage_function_parent(node, &parent)) CM_Enable_DevNode(parent, 0);
  result = CM_Enable_DevNode(node, 0);
  ULONG status = 0;
  ULONG problem = 0;
  if (node_is_disabled(node, &status, &problem)) {
    unsigned long setup_result = 0;
    if (change_state(instance_id, DICS_ENABLE, &setup_result)) result = CR_SUCCESS;
  }
  if (result_code != nullptr) *result_code = result;
  return result == CR_SUCCESS;
#endif
}

bool WindowsDeviceEnforcer::is_disabled(
    const std::wstring& instance_id,
    bool* disabled,
    unsigned long* result_code,
    unsigned long* status_bits,
    unsigned long* problem_code,
    std::wstring* diagnostic) const {
  if (disabled == nullptr) return false;
#ifdef USB_DLP_XP_FILTER
  if (uses_filter(instance_id)) {
    bool found = false;
    bool allowed = true;
    unsigned long count = 0;
    const bool success = filter_request(instance_id, false, false, &found,
                                        &allowed, result_code, &count, diagnostic);
    if (status_bits != nullptr) *status_bits = count;
    if (problem_code != nullptr) *problem_code = 0;
    if (success) *disabled = !allowed;
    return success;
  }
#endif
  (void)diagnostic;
  DEVINST node = 0;
  CONFIGRET result = CR_FAILURE;
  ULONG status = 0;
  ULONG problem = 0;
  const bool located = locate(instance_id, &node, &result);
  if (located) result = CM_Get_DevNode_Status(&status, &problem, node, 0);
  if (located && result == CR_SUCCESS) {
    if (result_code != nullptr) *result_code = result;
    if (status_bits != nullptr) *status_bits = status;
    if (problem_code != nullptr) *problem_code = problem;
    *disabled = (status & DN_HAS_PROBLEM) != 0 && problem == CM_PROB_DISABLED;
    return true;
  }
#ifndef USB_DLP_XP_SETUPAPI
  // The disk child vanishes once its USB function parent is disabled, so a plain
  // status query fails. That is a blocked device, not an unknown one.
  DEVINST phantom = 0;
  std::vector<wchar_t> phantom_id(instance_id.begin(), instance_id.end());
  phantom_id.push_back(L'\0');
  if (CM_Locate_DevNodeW(&phantom, &phantom_id[0], CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS) {
    DEVINST parent = 0;
    if (storage_function_parent(phantom, &parent) &&
        node_is_disabled(parent, status_bits, problem_code)) {
      *disabled = true;
      if (result_code != nullptr) *result_code = CR_SUCCESS;
      return true;
    }
  }
#endif
  if (result_code != nullptr) *result_code = result;
  if (status_bits != nullptr) *status_bits = 0;
  if (problem_code != nullptr) *problem_code = 0;
  if (result != CR_SUCCESS) return false;
  *disabled = (status & DN_HAS_PROBLEM) != 0 && problem == CM_PROB_DISABLED;
  return true;
}

}  // namespace usb_dlp
