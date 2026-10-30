#include "usb_dlp/windows_device.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <vector>

namespace usb_dlp {
namespace {

class DeviceInfoSet {
 public:
  explicit DeviceInfoSet(HDEVINFO handle) : handle_(handle) {}
  ~DeviceInfoSet() {
    if (handle_ != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(handle_);
  }
  HDEVINFO get() const { return handle_; }

 private:
  HDEVINFO handle_;
  DeviceInfoSet(const DeviceInfoSet&);
  DeviceInfoSet& operator=(const DeviceInfoSet&);
};

std::wstring lower(const std::wstring& value) {
  std::wstring result(value);
  std::transform(result.begin(), result.end(), result.begin(), towlower);
  return result;
}

std::wstring property_string(HDEVINFO set, SP_DEVINFO_DATA* info, DWORD property) {
  DWORD type = 0;
  DWORD needed = 0;
  SetupDiGetDeviceRegistryPropertyW(set, info, property, &type, nullptr, 0, &needed);
  if (needed == 0 || (type != REG_SZ && type != REG_EXPAND_SZ)) return std::wstring();
  std::vector<BYTE> buffer(needed + sizeof(wchar_t), 0);
  if (!SetupDiGetDeviceRegistryPropertyW(
          set, info, property, &type, &buffer[0], static_cast<DWORD>(buffer.size()), &needed)) {
    return std::wstring();
  }
  return reinterpret_cast<const wchar_t*>(&buffer[0]);
}

std::vector<std::wstring> property_multi_string(
    HDEVINFO set,
    SP_DEVINFO_DATA* info,
    DWORD property) {
  DWORD type = 0;
  DWORD needed = 0;
  SetupDiGetDeviceRegistryPropertyW(set, info, property, &type, nullptr, 0, &needed);
  std::vector<std::wstring> values;
  if (needed == 0 || type != REG_MULTI_SZ) return values;
  std::vector<BYTE> buffer(needed + (2 * sizeof(wchar_t)), 0);
  if (!SetupDiGetDeviceRegistryPropertyW(
          set, info, property, &type, &buffer[0], static_cast<DWORD>(buffer.size()), &needed)) {
    return values;
  }
  const wchar_t* cursor = reinterpret_cast<const wchar_t*>(&buffer[0]);
  while (*cursor != L'\0') {
    values.push_back(cursor);
    cursor += values.back().size() + 1;
  }
  return values;
}

std::wstring instance_id(HDEVINFO set, SP_DEVINFO_DATA* info) {
  DWORD needed = 0;
  SetupDiGetDeviceInstanceIdW(set, info, nullptr, 0, &needed);
  if (needed == 0) return std::wstring();
  std::vector<wchar_t> buffer(needed + 1, L'\0');
  if (!SetupDiGetDeviceInstanceIdW(set, info, &buffer[0], static_cast<DWORD>(buffer.size()), nullptr)) {
    return std::wstring();
  }
  return &buffer[0];
}

std::wstring devnode_id(DEVINST node) {
  ULONG length = 0;
  if (CM_Get_Device_ID_Size(&length, node, 0) != CR_SUCCESS) return std::wstring();
  std::vector<wchar_t> buffer(length + 2, L'\0');
  if (CM_Get_Device_IDW(node, &buffer[0], static_cast<ULONG>(buffer.size()), 0) != CR_SUCCESS) {
    return std::wstring();
  }
  return &buffer[0];
}

std::wstring class_guid_for_node(DEVINST node) {
  wchar_t buffer[64] = {};
  ULONG size = sizeof(buffer);
  ULONG type = 0;
  if (CM_Get_DevNode_Registry_PropertyW(
          node, CM_DRP_CLASSGUID, &type, buffer, &size, 0) != CR_SUCCESS) {
    return std::wstring();
  }
  return buffer;
}

void add_ancestry(
    DEVINST start,
    std::vector<std::string>* class_guids,
    std::vector<std::wstring>* ids) {
  DEVINST current = start;
  for (int depth = 0; depth < 16; ++depth) {
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS) break;
    current = parent;
    const std::wstring id = devnode_id(current);
    if (!id.empty()) ids->push_back(id);
    const std::wstring class_guid = class_guid_for_node(current);
    if (!class_guid.empty()) class_guids->push_back(wide_to_utf8(class_guid));
  }
}

bool extract_hex_id(const std::wstring& source, const wchar_t* marker, std::string* value) {
  const std::wstring lowered = lower(source);
  const std::wstring lowered_marker = lower(marker);
  const std::wstring::size_type position = lowered.find(lowered_marker);
  if (position == std::wstring::npos || position + lowered_marker.size() + 4 > source.size()) return false;
  const std::wstring candidate = source.substr(position + lowered_marker.size(), 4);
  for (wchar_t character : candidate) {
    if (!iswxdigit(character)) return false;
  }
  *value = wide_to_utf8(candidate);
  std::transform(value->begin(), value->end(), value->begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return true;
}

bool find_vid_pid(
    const std::wstring& own_id,
    const std::vector<std::wstring>& hardware_ids,
    const std::vector<std::wstring>& ancestry,
    std::string* vid,
    std::string* pid) {
  std::vector<std::wstring> candidates;
  candidates.push_back(own_id);
  candidates.insert(candidates.end(), hardware_ids.begin(), hardware_ids.end());
  candidates.insert(candidates.end(), ancestry.begin(), ancestry.end());
  for (const std::wstring& candidate : candidates) {
    std::string found_vid;
    std::string found_pid;
    if (extract_hex_id(candidate, L"vid_", &found_vid) &&
        extract_hex_id(candidate, L"pid_", &found_pid)) {
      *vid = found_vid;
      *pid = found_pid;
      return true;
    }
  }
  return false;
}

}  // namespace

bool WindowsDeviceEnumerator::enumerate(
    std::vector<WindowsDevice>* devices,
    std::wstring* error,
    std::vector<std::wstring>* diagnostics) const {
  if (devices == nullptr) return false;
  devices->clear();
  if (diagnostics != nullptr) diagnostics->clear();
  DeviceInfoSet set(SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT));
  if (set.get() == INVALID_HANDLE_VALUE) {
    if (error != nullptr) *error = L"SetupDiGetClassDevs failed";
    return false;
  }

  for (DWORD index = 0;; ++index) {
    SP_DEVINFO_DATA info = {};
    info.cbSize = sizeof(info);
    if (!SetupDiEnumDeviceInfo(set.get(), index, &info)) {
      if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
      continue;
    }

    const std::wstring id = instance_id(set.get(), &info);
    const std::wstring service = property_string(set.get(), &info, SPDRP_SERVICE);
    const std::wstring class_guid = property_string(set.get(), &info, SPDRP_CLASSGUID);
    const std::wstring lowered_service = lower(service);
    const std::wstring lowered_id = lower(id);
    // The USBSTOR service belongs to the USB parent on XP; only its disk child
    // is a safe enforcement target and covers every volume on the stick.
    const bool storage = lowered_id.find(L"usbstor\\") == 0;
    const bool wpd = lower(class_guid) == L"{eec5ad98-8080-425f-922a-dabf3de3f69a}";
    const bool usb_candidate = lowered_id.find(L"usb\\") == 0 || storage || wpd;
    if (!storage && !wpd) {
      if (diagnostics != nullptr && usb_candidate) {
        diagnostics->push_back(
            L"candidate instance_id=" + id + L" service=" +
            (service.empty() ? std::wstring(L"unknown") : service) + L" class_guid=" +
            (class_guid.empty() ? std::wstring(L"unknown") : class_guid) +
            L" governed=false reason=not_storage_or_wpd");
      }
      continue;
    }

    std::vector<std::wstring> ancestry_ids;
    std::vector<std::string> ancestry_classes;
    add_ancestry(info.DevInst, &ancestry_classes, &ancestry_ids);
    const std::vector<std::wstring> hardware_ids =
        property_multi_string(set.get(), &info, SPDRP_HARDWAREID);
    std::string vid;
    std::string pid;
    // XP does not consistently expose the USB parent to a USBSTOR child while
    // the device stack is still starting. Keep the governed device in the
    // default-deny path even when VID/PID enrichment is temporarily unavailable.
    find_vid_pid(id, hardware_ids, ancestry_ids, &vid, &pid);

    if (diagnostics != nullptr) {
      diagnostics->push_back(
          L"candidate instance_id=" + id + L" service=" +
          (service.empty() ? std::wstring(L"unknown") : service) + L" class_guid=" +
          (class_guid.empty() ? std::wstring(L"unknown") : class_guid) + L" governed=true vid=" +
          (vid.empty() ? std::wstring(L"unknown") : utf8_to_wide(vid)) + L" pid=" +
          (pid.empty() ? std::wstring(L"unknown") : utf8_to_wide(pid)) + L" ancestry=" +
          std::to_wstring(ancestry_ids.size()));
    }

    WindowsDevice output;
    output.instance_id = id;
    output.friendly_name = property_string(set.get(), &info, SPDRP_FRIENDLYNAME);
    if (output.friendly_name.empty()) {
      output.friendly_name = property_string(set.get(), &info, SPDRP_DEVICEDESC);
    }
    output.policy_device.instance_id = wide_to_utf8(id);
    output.policy_device.vid = vid;
    output.policy_device.pid = pid;
    output.policy_device.device_class =
        storage ? ProductDeviceClass::usb_storage : ProductDeviceClass::wpd;
    output.policy_device.class_guid = wide_to_utf8(class_guid);
    output.policy_device.ancestry_class_guids = ancestry_classes;
    devices->push_back(output);
  }
  return true;
}

std::wstring common_application_data_path() {
  wchar_t path[MAX_PATH] = {};
  if (SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path) != S_OK) {
    return std::wstring();
  }
  return path;
}

std::string wide_to_utf8(const std::wstring& value) {
  if (value.empty()) return std::string();
  // XP only accepts zero flags for CP_UTF8 conversions.
#if _WIN32_WINNT >= 0x0600
  const DWORD conversion_flags = WC_ERR_INVALID_CHARS;
#else
  const DWORD conversion_flags = 0;
#endif
  const int size = WideCharToMultiByte(
      CP_UTF8, conversion_flags, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) return std::string();
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, conversion_flags, value.c_str(), static_cast<int>(value.size()),
                      &result[0], size, nullptr, nullptr);
  return result;
}

std::wstring utf8_to_wide(const std::string& value) {
  if (value.empty()) return std::wstring();
#if _WIN32_WINNT >= 0x0600
  const DWORD conversion_flags = MB_ERR_INVALID_CHARS;
#else
  const DWORD conversion_flags = 0;
#endif
  const int size = MultiByteToWideChar(
      CP_UTF8, conversion_flags, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return std::wstring();
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, conversion_flags, value.c_str(), static_cast<int>(value.size()),
                      &result[0], size);
  return result;
}

}  // namespace usb_dlp
