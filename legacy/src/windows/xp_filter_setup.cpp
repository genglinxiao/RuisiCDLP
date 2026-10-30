#include <windows.h>
#include <winioctl.h>

#include "usb_dlp/xp_filter_protocol.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const wchar_t* const kService = L"RuisiUsbDlpFilter";
const wchar_t* const kClassKey =
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e967-e325-11ce-bfc1-08002be10318}";

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

int install(const wchar_t* source) {
  OSVERSIONINFOEXW version = {};
  version.dwOSVersionInfoSize = sizeof(version);
  if (!GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&version)) ||
      version.dwMajorVersion != 5 || version.dwMinorVersion != 1 ||
      version.wServicePackMajor < 3) {
    std::fwprintf(stderr, L"This filter track requires Windows XP SP3 x86.\n");
    return ERROR_OLD_WIN_VERSION;
  }
  wchar_t system_root[MAX_PATH] = {};
  wchar_t volume_root[MAX_PATH] = {};
  wchar_t file_system[32] = {};
  if (GetWindowsDirectoryW(system_root, MAX_PATH) == 0 ||
      !GetVolumePathNameW(system_root, volume_root, MAX_PATH) ||
      !GetVolumeInformationW(volume_root, nullptr, 0, nullptr, nullptr,
                             nullptr, file_system, 32) ||
      _wcsicmp(file_system, L"NTFS") != 0) {
    std::fwprintf(stderr, L"The XP system volume must be NTFS.\n");
    return ERROR_NOT_SUPPORTED;
  }
  HKEY key = nullptr;
  LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kClassKey, 0,
                              KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
  if (result != ERROR_SUCCESS) return static_cast<int>(result);
  std::vector<std::wstring> names;
  if (!read_filters(key, &names)) {
    RegCloseKey(key);
    std::fwprintf(stderr, L"Refusing malformed disk-class UpperFilters.\n");
    return ERROR_INVALID_DATA;
  }
  for (const std::wstring& name : names) {
    if (_wcsicmp(name.c_str(), kService) == 0) {
      RegCloseKey(key);
      std::fwprintf(stderr, L"Filter already registered. Refusing overwrite.\n");
      return ERROR_ALREADY_EXISTS;
    }
  }
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (manager == nullptr) { RegCloseKey(key); return static_cast<int>(GetLastError()); }
  SC_HANDLE existing = OpenServiceW(manager, kService, SERVICE_QUERY_STATUS);
  if (existing != nullptr) {
    CloseServiceHandle(existing);
    CloseServiceHandle(manager);
    RegCloseKey(key);
    return ERROR_ALREADY_EXISTS;
  }
  const std::wstring target = driver_path();
  if (target.empty() || !CopyFileW(source, target.c_str(), TRUE)) {
    const int error = static_cast<int>(GetLastError());
    CloseServiceHandle(manager);
    RegCloseKey(key);
    return error;
  }
  SC_HANDLE service = CreateServiceW(manager, kService, L"Ruisi USB DLP XP disk filter",
      SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
      SERVICE_ERROR_NORMAL, L"\\SystemRoot\\System32\\drivers\\RuisiUsbDlpFilter.sys",
      nullptr, nullptr, nullptr, nullptr, nullptr);
  if (service == nullptr) {
    const int error = static_cast<int>(GetLastError());
    DeleteFileW(target.c_str());
    CloseServiceHandle(manager);
    RegCloseKey(key);
    return error;
  }
  names.push_back(kService);
  if (!write_filters(key, names)) {
    DeleteService(service);
    DeleteFileW(target.c_str());
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    RegCloseKey(key);
    return ERROR_WRITE_FAULT;
  }
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  RegCloseKey(key);
  std::wprintf(L"Filter registered. Restart XP before testing.\n");
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
  if (argc == 2 && _wcsicmp(argv[1], L"uninstall") == 0) return uninstall();
  if (argc == 2 && _wcsicmp(argv[1], L"diagnose") == 0) return diagnose();
  std::fwprintf(stderr, L"Usage: xp-filter-setup.exe install <driver.sys> | uninstall | diagnose\n");
  return ERROR_INVALID_PARAMETER;
}
