#include <windows.h>
#include <winioctl.h>

#include "usb_dlp/xp_filter_protocol.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const wchar_t* const kService = L"RuisiUsbDlpFilter";
const wchar_t* const kServiceKey =
    L"SYSTEM\\CurrentControlSet\\Services\\RuisiUsbDlpFilter";
const wchar_t* const kClassKey =
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e967-e325-11ce-bfc1-08002be10318}";

// A disk-class upper filter that is not loaded in the same load-order group as
// PartMgr makes the boot volume stack incomplete.  On XP that is an immediate
// STOP 0x7B (INACCESSIBLE_BOOT_DEVICE, second parameter 0xC0000034) before the
// desktop ever appears, and it cannot be undone from the affected machine.
// PartMgr belongs to "System Bus Extender"; the filter must match it.
const wchar_t* const kBootGroup = L"System Bus Extender";
const wchar_t* const kVolumeManagerFilter = L"PartMgr";

bool read_filters(HKEY key, std::vector<std::wstring>* names) {
  DWORD type = 0;
  DWORD bytes = 0;
  LONG result = RegQueryValueExW(key, L"UpperFilters", nullptr, &type, nullptr, &bytes);
  if (result == ERROR_FILE_NOT_FOUND) return true;
  if (result != ERROR_SUCCESS || type != REG_MULTI_SZ || bytes < 2 * sizeof(wchar_t) ||
      bytes % sizeof(wchar_t) != 0 || bytes > 64 * 1024) return false;
  std::vector<wchar_t> value(bytes / sizeof(wchar_t));
  result = RegQueryValueExW(key, L"UpperFilters", nullptr, &type,
                            reinterpret_cast<BYTE*>(&value[0]), &bytes);
  if (result != ERROR_SUCCESS || type != REG_MULTI_SZ ||
      value[value.size() - 1] != L'\0' || value[value.size() - 2] != L'\0') return false;
  std::size_t pos = 0;
  while (pos + 1 < value.size()) {
    if (value[pos] == L'\0') return pos + 2 == value.size();
    std::size_t end = pos;
    while (end < value.size() && value[end] != L'\0') ++end;
    if (end + 1 >= value.size()) return false;
    names->push_back(std::wstring(&value[pos], end - pos));
    pos = end + 1;
  }
  return pos + 1 == value.size();
}

bool write_filters(HKEY key, const std::vector<std::wstring>& names) {
  std::vector<wchar_t> value;
  for (const std::wstring& name : names) {
    value.insert(value.end(), name.begin(), name.end());
    value.push_back(L'\0');
  }
  value.push_back(L'\0');
  if (value.size() == 1) value.push_back(L'\0');
  return RegSetValueExW(key, L"UpperFilters", 0, REG_MULTI_SZ,
                        reinterpret_cast<const BYTE*>(&value[0]),
                        static_cast<DWORD>(value.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}

std::wstring driver_path() {
  wchar_t system[MAX_PATH] = {};
  if (GetSystemDirectoryW(system, MAX_PATH) == 0) return std::wstring();
  return std::wstring(system) + L"\\drivers\\RuisiUsbDlpFilter.sys";
}

bool read_service_string(const wchar_t* name, std::wstring* out) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kServiceKey, 0, KEY_QUERY_VALUE, &key) !=
      ERROR_SUCCESS) return false;
  DWORD type = 0;
  DWORD bytes = 0;
  LONG result = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
  if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
      bytes < sizeof(wchar_t)) {
    RegCloseKey(key);
    SetLastError(result == ERROR_SUCCESS ? ERROR_INVALID_DATA : static_cast<DWORD>(result));
    return false;
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  result = RegQueryValueExW(key, name, nullptr, &type,
                            reinterpret_cast<BYTE*>(&buffer[0]), &bytes);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS) {
    SetLastError(static_cast<DWORD>(result));
    return false;
  }
  buffer[bytes / sizeof(wchar_t)] = L'\0';
  *out = std::wstring(&buffer[0]);
  return true;
}

bool write_service_string(const wchar_t* name, const wchar_t* data) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kServiceKey, 0, KEY_SET_VALUE, &key) !=
      ERROR_SUCCESS) return false;
  const DWORD bytes = static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t));
  const LONG result = RegSetValueExW(key, name, 0, REG_SZ,
                                     reinterpret_cast<const BYTE*>(data), bytes);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS) SetLastError(static_cast<DWORD>(result));
  return result == ERROR_SUCCESS;
}

struct BootState {
  bool service_present = false;
  DWORD start = 0xFFFFFFFFu;
  bool group_present = false;
  std::wstring group;

  bool boot_safe() const {
    return service_present && start == SERVICE_BOOT_START && group_present &&
           _wcsicmp(group.c_str(), kBootGroup) == 0;
  }
};

bool query_boot_state(BootState* state) {
  *state = BootState();
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) return false;
  SC_HANDLE service = OpenServiceW(manager, kService, SERVICE_QUERY_CONFIG);
  if (service == nullptr) {
    const DWORD error = GetLastError();
    CloseServiceHandle(manager);
    if (error == ERROR_SERVICE_DOES_NOT_EXIST) return true;
    SetLastError(error);
    return false;
  }
  state->service_present = true;
  DWORD needed = 0;
  QueryServiceConfigW(service, nullptr, 0, &needed);
  std::vector<BYTE> buffer(needed == 0 ? 4096 : needed, 0);
  LPQUERY_SERVICE_CONFIGW config = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(&buffer[0]);
  if (QueryServiceConfigW(service, config, static_cast<DWORD>(buffer.size()), &needed))
    state->start = config->dwStartType;
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  state->group_present = read_service_string(L"Group", &state->group);
  return true;
}

// Forces Start=SERVICE_BOOT_START and Group=kBootGroup, then re-reads both and
// returns the observed state.  Never trust the API call result alone.
bool apply_boot_state(BootState* after) {
  BootState current;
  if (!query_boot_state(&current) || !current.service_present) {
    SetLastError(ERROR_SERVICE_DOES_NOT_EXIST);
    return false;
  }
  const bool start_ok = current.start == SERVICE_BOOT_START;
  const bool group_ok = current.group_present &&
                        _wcsicmp(current.group.c_str(), kBootGroup) == 0;
  if (!start_ok || !group_ok) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) return false;
    SC_HANDLE service =
        OpenServiceW(manager, kService, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
    if (service == nullptr) {
      const DWORD error = GetLastError();
      CloseServiceHandle(manager);
      SetLastError(error);
      return false;
    }
    const BOOL changed = ChangeServiceConfigW(
        service, SERVICE_NO_CHANGE, SERVICE_BOOT_START, SERVICE_NO_CHANGE,
        nullptr, group_ok ? nullptr : kBootGroup, nullptr, nullptr, nullptr,
        nullptr, nullptr);
    const DWORD change_error = changed ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!changed) {
      SetLastError(change_error);
      return false;
    }
  }
  // Some XP service-control stacks ignore lpLoadOrderGroup on ChangeServiceConfig
  // for drivers created with a null group, so fall back to the registry value.
  BootState observed;
  if (!query_boot_state(&observed)) return false;
  if (observed.service_present &&
      (!observed.group_present || _wcsicmp(observed.group.c_str(), kBootGroup) != 0)) {
    if (!write_service_string(L"Group", kBootGroup)) return false;
    if (!query_boot_state(&observed)) return false;
  }
  *after = observed;
  return observed.boot_safe();
}

bool filters_registered_from_class_key(bool* malformed) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kClassKey, 0, KEY_QUERY_VALUE, &key) !=
      ERROR_SUCCESS) {
    if (malformed != nullptr) *malformed = true;
    return false;
  }
  std::vector<std::wstring> names;
  const bool ok = read_filters(key, &names);
  RegCloseKey(key);
  if (malformed != nullptr) *malformed = !ok;
  if (!ok) return false;
  for (const std::wstring& name : names)
    if (_wcsicmp(name.c_str(), kService) == 0) return true;
  return false;
}

int preflight_os() {
  // GetVersionExW is deprecated when built against the Windows 10 SDK, but it is
  // the supported way to detect XP SP3 at runtime and this track only targets XP.
  // The Windows 7.1A SDK used for the original 1.1.8 build did not raise C4996.
#pragma warning(push)
#pragma warning(disable : 4996)
  OSVERSIONINFOEXW version = {};
  version.dwOSVersionInfoSize = sizeof(version);
  if (!GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&version)) ||
      version.dwMajorVersion != 5 || version.dwMinorVersion != 1 ||
      version.wServicePackMajor < 3) {
    std::fwprintf(stderr, L"This filter track requires Windows XP SP3 x86.\n");
    return ERROR_OLD_WIN_VERSION;
  }
#pragma warning(pop)
  wchar_t windows_root[MAX_PATH] = {};
  wchar_t volume_root[MAX_PATH] = {};
  wchar_t file_system[32] = {};
  if (GetWindowsDirectoryW(windows_root, MAX_PATH) == 0 ||
      !GetVolumePathNameW(windows_root, volume_root, MAX_PATH) ||
      !GetVolumeInformationW(volume_root, nullptr, 0, nullptr, nullptr, nullptr,
                             file_system, 32) ||
      _wcsicmp(file_system, L"NTFS") != 0) {
    std::fwprintf(stderr, L"The XP system volume must be NTFS.\n");
    return ERROR_NOT_SUPPORTED;
  }
  return 0;
}

void print_state(const wchar_t* label, const BootState& state) {
  if (!state.service_present) {
    std::wprintf(L"%ls: service_missing\n", label);
    return;
  }
  std::wprintf(L"%ls: start=%lu (%ls) group=%ls (%ls)\n", label,
               state.start,
               state.start == SERVICE_BOOT_START ? L"BOOT_START" : L"NOT_BOOT_SAFE",
               state.group_present ? state.group.c_str() : L"<unset>",
               state.group_present && _wcsicmp(state.group.c_str(), kBootGroup) == 0
                   ? L"boot_safe" : L"NOT_BOOT_SAFE");
}

int install(const wchar_t* source) {
  const int preflight = preflight_os();
  if (preflight != 0) return preflight;

  BootState state;
  if (!query_boot_state(&state)) {
    std::fwprintf(stderr, L"Cannot query service manager: %lu\n", GetLastError());
    return static_cast<int>(GetLastError());
  }
  if (state.service_present) {
    std::fwprintf(stderr,
                  L"Service %ls already exists. Run \"repair\" to normalise its boot "
                  L"configuration, or \"uninstall\" first.\n", kService);
    return ERROR_ALREADY_EXISTS;
  }
  bool malformed = false;
  if (filters_registered_from_class_key(&malformed)) {
    std::fwprintf(stderr,
                  L"UpperFilters already references %ls without a service. That state "
                  L"cannot boot. Run \"repair\" to remove the dangling entry, then "
                  L"reboot before installing again.\n", kService);
    return ERROR_ALREADY_EXISTS;
  }
  if (malformed) {
    std::fwprintf(stderr, L"Refusing malformed disk-class UpperFilters.\n");
    return ERROR_INVALID_DATA;
  }

  const std::wstring target = driver_path();
  if (target.empty()) {
    std::fwprintf(stderr, L"Cannot resolve the driver directory.\n");
    return ERROR_PATH_NOT_FOUND;
  }
  // CopyFileW(..., TRUE) silently fails when the file already exists; the old
  // code returned without printing anything, which hid failed upgrades.
  if (!CopyFileW(source, target.c_str(), FALSE)) {
    const DWORD error = GetLastError();
    std::fwprintf(stderr, L"Cannot copy %ls to %ls: error %lu\n", source,
                  target.c_str(), error);
    return static_cast<int>(error);
  }
  std::wprintf(L"driver_file=%ls\n", target.c_str());

  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (manager == nullptr) {
    const DWORD error = GetLastError();
    DeleteFileW(target.c_str());
    std::fwprintf(stderr, L"Cannot open the service manager: %lu\n", error);
    return static_cast<int>(error);
  }
  SC_HANDLE service = CreateServiceW(
      manager, kService, L"Ruisi USB DLP XP disk filter", SERVICE_ALL_ACCESS,
      SERVICE_KERNEL_DRIVER, SERVICE_BOOT_START, SERVICE_ERROR_NORMAL,
      L"\\SystemRoot\\System32\\drivers\\RuisiUsbDlpFilter.sys", kBootGroup,
      nullptr, nullptr, nullptr, nullptr);
  if (service == nullptr) {
    const DWORD error = GetLastError();
    DeleteFileW(target.c_str());
    CloseServiceHandle(manager);
    std::fwprintf(stderr, L"Cannot create driver service: error %lu\n", error);
    return static_cast<int>(error);
  }
  CloseServiceHandle(service);
  CloseServiceHandle(manager);

  // Nothing may touch UpperFilters until the service is proven boot-safe.
  BootState after;
  if (!apply_boot_state(&after)) {
    const DWORD error = GetLastError();
    print_state(L"service_config", after);
    std::fwprintf(stderr,
                  L"Service is not BOOT_START in group \"%ls\". Refusing to register "
                  L"the disk filter: that combination is exactly what produces "
                  L"STOP 0x7B on the next boot.\n", kBootGroup);
    SC_HANDLE cleanup_manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (cleanup_manager != nullptr) {
      SC_HANDLE cleanup =
          OpenServiceW(cleanup_manager, kService, DELETE | SERVICE_STOP);
      if (cleanup != nullptr) {
        DeleteService(cleanup);
        CloseServiceHandle(cleanup);
      }
      CloseServiceHandle(cleanup_manager);
    }
    DeleteFileW(target.c_str());
    return error == ERROR_SUCCESS ? ERROR_INVALID_DATA : static_cast<int>(error);
  }
  print_state(L"service_config", after);

  HKEY key = nullptr;
  LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kClassKey, 0,
                              KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
  if (result != ERROR_SUCCESS) {
    std::fwprintf(stderr, L"Cannot open the disk class key: %ld\n", result);
    return static_cast<int>(result);
  }
  std::vector<std::wstring> names;
  if (!read_filters(key, &names)) {
    RegCloseKey(key);
    std::fwprintf(stderr, L"Refusing malformed disk-class UpperFilters.\n");
    return ERROR_INVALID_DATA;
  }
  if (names.empty()) names.push_back(kVolumeManagerFilter);
  names.push_back(kService);
  if (!write_filters(key, names)) {
    RegCloseKey(key);
    std::fwprintf(stderr, L"Cannot write disk-class UpperFilters.\n");
    return ERROR_WRITE_FAULT;
  }
  RegCloseKey(key);

  bool malformed_after = false;
  if (!filters_registered_from_class_key(&malformed_after)) {
    std::fwprintf(stderr, L"UpperFilters verification failed after write.\n");
    return ERROR_WRITE_FAULT;
  }
  std::wprintf(L"Filter registered with a boot-safe service configuration.\n");
  std::wprintf(L"Reboot XP now, then run \"diagnose\" before inserting any USB disk.\n");
  return 0;
}

// Fixes machines that already carry the 1.1.8 layout (SERVICE_DEMAND_START with
// the filter listed in UpperFilters).  Those machines blue-screen on the next
// boot; they must be repaired before they are restarted.
int repair() {
  const int preflight = preflight_os();
  if (preflight != 0) return preflight;

  BootState state;
  if (!query_boot_state(&state)) {
    std::fwprintf(stderr, L"Cannot query service manager: %lu\n", GetLastError());
    return static_cast<int>(GetLastError());
  }
  print_state(L"before", state);

  bool malformed = false;
  const bool registered = filters_registered_from_class_key(&malformed);
  if (malformed) {
    std::fwprintf(stderr, L"Refusing malformed disk-class UpperFilters.\n");
    return ERROR_INVALID_DATA;
  }
  const std::wstring path = driver_path();
  const bool file_ok = !path.empty() &&
                       GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
  std::wprintf(L"driver_file=%ls\n", file_ok ? L"present" : L"missing");

  if (!state.service_present) {
    if (!registered) {
      std::fwprintf(stderr, L"Nothing to repair: no service and no filter entry.\n");
      return ERROR_SERVICE_DOES_NOT_EXIST;
    }
    HKEY key = nullptr;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kClassKey, 0,
                                KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (result != ERROR_SUCCESS) return static_cast<int>(result);
    std::vector<std::wstring> names;
    if (!read_filters(key, &names)) {
      RegCloseKey(key);
      return ERROR_INVALID_DATA;
    }
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::wstring& name) {
      return _wcsicmp(name.c_str(), kService) == 0;
    }), names.end());
    if (!write_filters(key, names)) {
      RegCloseKey(key);
      return ERROR_WRITE_FAULT;
    }
    RegCloseKey(key);
    std::wprintf(L"Removed a dangling UpperFilters entry left by a failed install.\n");
    std::wprintf(L"Reboot XP now; the disk filter is no longer attached.\n");
    return 0;
  }
  if (!file_ok) {
    std::fwprintf(stderr,
                  L"%ls is missing while the filter service exists. Restore the file "
                  L"or run \"uninstall\"; do not reboot this machine as-is.\n",
                  path.c_str());
    return ERROR_FILE_NOT_FOUND;
  }

  BootState after;
  if (!apply_boot_state(&after)) {
    const DWORD error = GetLastError();
    print_state(L"after", after);
    std::fwprintf(stderr, L"Cannot enforce a boot-safe service configuration.\n");
    return error == ERROR_SUCCESS ? ERROR_INVALID_DATA : static_cast<int>(error);
  }
  print_state(L"after", after);

  if (!registered) {
    HKEY key = nullptr;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kClassKey, 0,
                                KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (result != ERROR_SUCCESS) return static_cast<int>(result);
    std::vector<std::wstring> names;
    if (!read_filters(key, &names)) {
      RegCloseKey(key);
      return ERROR_INVALID_DATA;
    }
    if (names.empty()) names.push_back(kVolumeManagerFilter);
    names.push_back(kService);
    if (!write_filters(key, names)) {
      RegCloseKey(key);
      return ERROR_WRITE_FAULT;
    }
    RegCloseKey(key);
    std::wprintf(L"disk_upper_filter=restored\n");
  } else {
    std::wprintf(L"disk_upper_filter=present\n");
  }
  std::wprintf(L"Repair complete. Reboot XP, then run \"diagnose\".\n");
  return 0;
}

int uninstall() {
  HKEY key = nullptr;
  LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kClassKey, 0,
                              KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
  if (result != ERROR_SUCCESS) return static_cast<int>(result);
  std::vector<std::wstring> names;
  if (!read_filters(key, &names)) {
    RegCloseKey(key);
    return ERROR_INVALID_DATA;
  }
  const std::size_t before = names.size();
  names.erase(std::remove_if(names.begin(), names.end(), [](const std::wstring& name) {
    return _wcsicmp(name.c_str(), kService) == 0;
  }), names.end());
  if (names.size() != before && !write_filters(key, names)) {
    RegCloseKey(key);
    return ERROR_WRITE_FAULT;
  }
  RegCloseKey(key);
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) return static_cast<int>(GetLastError());
  SC_HANDLE service = OpenServiceW(manager, kService, DELETE);
  if (service != nullptr) {
    if (!DeleteService(service)) {
      const int error = static_cast<int>(GetLastError());
      CloseServiceHandle(service);
      CloseServiceHandle(manager);
      return error;
    }
    CloseServiceHandle(service);
  } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
    const int error = static_cast<int>(GetLastError());
    CloseServiceHandle(manager);
    return error;
  }
  CloseServiceHandle(manager);
  const std::wstring path = driver_path();
  if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
  std::wprintf(L"Filter unregistered. Restart XP to detach it.\n");
  return 0;
}

int diagnose() {
  bool healthy = true;
  const std::wstring path = driver_path();
  const DWORD attributes = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
  std::wprintf(L"driver_file=%ls\n", attributes == INVALID_FILE_ATTRIBUTES ? L"missing" : L"present");
  if (attributes == INVALID_FILE_ATTRIBUTES) healthy = false;

  HKEY key = nullptr;
  LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kClassKey, 0, KEY_QUERY_VALUE, &key);
  std::vector<std::wstring> names;
  const bool filters_ok = result == ERROR_SUCCESS && read_filters(key, &names);
  if (key != nullptr) RegCloseKey(key);
  bool registered = false;
  for (const std::wstring& name : names)
    if (_wcsicmp(name.c_str(), kService) == 0) registered = true;
  std::wprintf(L"disk_upper_filter=%ls registry_result=%ld\n",
               registered ? L"present" : L"missing", result);
  if (!filters_ok || !registered) healthy = false;

  // A running filter that is not a BOOT_START driver in the PartMgr load-order
  // group will blue-screen the next cold or warm boot even though every runtime
  // probe below succeeds.  Fail early and loudly instead.
  BootState boot;
  if (!query_boot_state(&boot)) {
    std::wprintf(L"service_config=unavailable error=%lu\n", GetLastError());
    healthy = false;
  } else {
    print_state(L"service_config", boot);
    if (!boot.boot_safe()) {
      std::wprintf(L"boot_safety=FAIL_STOP_0X7B_ON_NEXT_BOOT\n");
      healthy = false;
    } else {
      std::wprintf(L"boot_safety=ok\n");
    }
  }

  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  SC_HANDLE service = manager == nullptr ? nullptr :
      OpenServiceW(manager, kService, SERVICE_QUERY_STATUS);
  const DWORD service_error = service == nullptr ? GetLastError() : ERROR_SUCCESS;
  SERVICE_STATUS status = {};
  const bool service_ok = service != nullptr && QueryServiceStatus(service, &status);
  std::wprintf(L"driver_service=%ls state=%lu error=%lu\n",
               service_ok ? L"present" : L"unavailable",
               service_ok ? status.dwCurrentState : 0,
               service_ok ? ERROR_SUCCESS : (service != nullptr ? GetLastError() : service_error));
  if (!service_ok || status.dwCurrentState != SERVICE_RUNNING) healthy = false;
  if (service != nullptr) CloseServiceHandle(service);
  if (manager != nullptr) CloseServiceHandle(manager);

  HANDLE control = CreateFileW(L"\\\\.\\RuisiUsbDlpFilter", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
  if (control == INVALID_HANDLE_VALUE) {
    std::wprintf(L"filter_control=unavailable error=%lu\n", GetLastError());
    healthy = false;
  } else {
    USB_DLP_FILTER_REQUEST request = {};
    USB_DLP_FILTER_REPLY reply = {};
    DWORD returned = 0;
    request.version = USB_DLP_FILTER_PROTOCOL_VERSION;
    wcscpy_s(request.physical_device_name, USB_DLP_FILTER_NAME_CAPACITY, L"RUISI_DIAGNOSTIC_PROBE");
    const BOOL ok = DeviceIoControl(control, IOCTL_USB_DLP_FILTER_QUERY,
                                    &request, sizeof(request), &reply, sizeof(reply),
                                    &returned, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    std::wprintf(L"filter_control=%ls ioctl_error=%lu attached_usb_disks=%lu\n",
                 ok && returned == sizeof(reply) && reply.version == USB_DLP_FILTER_PROTOCOL_VERSION
                     ? L"ready" : L"invalid",
                 error, ok ? reply.attached_usb_disks : 0);
    if (!ok || returned != sizeof(reply) || reply.version != USB_DLP_FILTER_PROTOCOL_VERSION)
      healthy = false;
    CloseHandle(control);
  }
  std::wprintf(L"filter_health=%ls\n", healthy ? L"READY_FOR_BYPASS_TEST" : L"FAIL_DO_NOT_TEST_AS_PROTECTED");
  return healthy ? 0 : ERROR_NOT_READY;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && _wcsicmp(argv[1], L"install") == 0) return install(argv[2]);
  if (argc == 2 && _wcsicmp(argv[1], L"repair") == 0) return repair();
  if (argc == 2 && _wcsicmp(argv[1], L"uninstall") == 0) return uninstall();
  if (argc == 2 && _wcsicmp(argv[1], L"diagnose") == 0) return diagnose();
  std::fwprintf(stderr,
                L"Usage: xp-filter-setup.exe install <driver.sys> | repair | uninstall | diagnose\n");
  return ERROR_INVALID_PARAMETER;
}
