#include "usb_dlp/windows_device.h"

#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <setupapi.h>

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

#ifdef USB_DLP_XP_SETUPAPI
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
  if (locate(instance_id, &node, &result)) result = CM_Disable_DevNode(node, 0);
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
  if (locate(instance_id, &node, &result)) result = CM_Enable_DevNode(node, 0);
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
  if (locate(instance_id, &node, &result)) {
    result = CM_Get_DevNode_Status(&status, &problem, node, 0);
  }
  if (result_code != nullptr) *result_code = result;
  if (status_bits != nullptr) *status_bits = result == CR_SUCCESS ? status : 0;
  if (problem_code != nullptr) *problem_code = result == CR_SUCCESS ? problem : 0;
  if (result != CR_SUCCESS) return false;
  *disabled = (status & DN_HAS_PROBLEM) != 0 && problem == CM_PROB_DISABLED;
  return true;
}

}  // namespace usb_dlp
