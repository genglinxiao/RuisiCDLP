#include "usb_dlp/agent_api.h"
#include "usb_dlp/approval.h"
#include "usb_dlp/approval_key.h"
#include "usb_dlp/crypto.h"
#include "usb_dlp/json.h"
#include "usb_dlp/notifier_bridge.h"
#include "usb_dlp/policy.h"
#include "usb_dlp/policy_json.h"
#include "usb_dlp/secure_store.h"
#include "usb_dlp/windows_device.h"
#ifndef USB_DLP_LEGACY_XP
#include "usb_dlp/policy_registry.h"
#endif

#include <windows.h>
#include <wincrypt.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

#ifndef USB_DLP_PROFILE
#define USB_DLP_PROFILE "host"
#endif

// Stamped by CMake at configure time so every log line identifies the binary
// that produced it. Two hours were lost today guessing which build was running.
#ifndef USB_DLP_BUILD_STAMP
// __DATE__/__TIME__ are evaluated at compile time, so the stamp always matches
// the binary that was actually built - unlike a configure-time timestamp.
#define USB_DLP_BUILD_STAMP __DATE__ " " __TIME__
#endif

const wchar_t* const kServiceName = L"UsbDlp.LegacyAgent";
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status = {};
HANDLE g_stop_event = nullptr;

std::wstring join_path(const std::wstring& parent, const wchar_t* child) {
  if (parent.empty()) return std::wstring();
  return parent + (parent[parent.size() - 1] == L'\\' ? L"" : L"\\") + child;
}

std::wstring module_directory() {
  std::vector<wchar_t> path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return std::wstring();
  std::wstring result(&path[0], length);
  const std::wstring::size_type separator = result.find_last_of(L"\\/");
  return separator == std::wstring::npos ? std::wstring() : result.substr(0, separator);
}

void log_line(const std::wstring& data_directory, const std::wstring& message) {
  const std::wstring path = join_path(data_directory, L"legacy-agent.log");
  WIN32_FILE_ATTRIBUTE_DATA attributes = {};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
    ULARGE_INTEGER size = {};
    size.LowPart = attributes.nFileSizeLow;
    size.HighPart = attributes.nFileSizeHigh;
    if (size.QuadPart >= 5ULL * 1024ULL * 1024ULL) {
      const std::wstring previous_path = path + L".1";
      DeleteFileW(previous_path.c_str());
      MoveFileW(path.c_str(), previous_path.c_str());
    }
  }
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME now = {};
  GetSystemTime(&now);
  wchar_t prefix[64] = {};
  wsprintfW(prefix, L"%04u-%02u-%02uT%02u:%02u:%02uZ ", now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond);
  const std::wstring line = std::wstring(prefix) + message + L"\r\n";
  const std::string utf8 = usb_dlp::wide_to_utf8(line);
  DWORD written = 0;
  WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
  CloseHandle(file);
}

std::wstring diagnostic_value(const std::string& value) {
  return value.empty() ? L"unknown" : usb_dlp::utf8_to_wide(value);
}

const wchar_t* device_class_name(usb_dlp::ProductDeviceClass device_class) {
  return device_class == usb_dlp::ProductDeviceClass::usb_storage ? L"usb_storage" : L"wpd";
}

const wchar_t* group_mode_name(usb_dlp::GroupMode group_mode) {
  return group_mode == usb_dlp::GroupMode::default_deny ? L"default_deny" : L"default_allow";
}

bool filter_usb_disk(const std::wstring& instance_id) {
#ifdef USB_DLP_XP_FILTER
  return instance_id.size() > 8 &&
      _wcsnicmp(instance_id.c_str(), L"USBSTOR\\", 8) == 0;
#else
  (void)instance_id;
  return false;
#endif
}

// GetPrivateProfile* only understands ANSI text. --configure writes the ini as
// UTF-16LE with a BOM, so every lookup silently fell back to its default:
// DryRun became true (agent only logged "would_block" and never disabled
// anything) and ServerUrl became empty. Parse the file here instead and accept
// ANSI, UTF-8 and UTF-16LE.
std::wstring read_config_text(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::wstring();
  const DWORD size = GetFileSize(file, nullptr);
  if (size == INVALID_FILE_SIZE || size == 0 || size > 256 * 1024) {
    CloseHandle(file);
    return std::wstring();
  }
  std::vector<char> raw(size, 0);
  DWORD read = 0;
  const BOOL read_ok = ReadFile(file, &raw[0], size, &read, nullptr);
  CloseHandle(file);
  if (!read_ok || read == 0) return std::wstring();
  raw.resize(read);
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&raw[0]);
  std::wstring text;
  if (raw.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
    const std::size_t count = (raw.size() - 2) / sizeof(wchar_t);
    text.assign(reinterpret_cast<const wchar_t*>(&raw[2]), count);
  } else {
    UINT codepage = CP_ACP;
    std::size_t offset = 0;
    if (raw.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
      codepage = CP_UTF8;
      offset = 3;
    }
    const int count = MultiByteToWideChar(codepage, 0, &raw[offset],
                                          static_cast<int>(raw.size() - offset), nullptr, 0);
    if (count <= 0) return std::wstring();
    text.resize(static_cast<std::size_t>(count));
    MultiByteToWideChar(codepage, 0, &raw[offset], static_cast<int>(raw.size() - offset),
                        &text[0], count);
  }
  if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
  return text;
}

std::wstring trim_copy(const std::wstring& value) {
  const std::wstring::size_type first = value.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return std::wstring();
  const std::wstring::size_type last = value.find_last_not_of(L" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::wstring read_ini_string(
    const std::wstring& ini,
    const wchar_t* key,
    const wchar_t* default_value) {
  const std::wstring text = read_config_text(ini);
  if (text.empty()) return default_value;
  bool in_section = false;
  std::wstring::size_type pos = 0;
  while (pos <= text.size()) {
    std::wstring::size_type end = text.find(L'\n', pos);
    if (end == std::wstring::npos) end = text.size();
    const std::wstring line = trim_copy(text.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
    if (line[0] == L'[') {
      const std::wstring::size_type close = line.find(L']');
      const std::wstring name =
          close == std::wstring::npos ? line.substr(1) : line.substr(1, close - 1);
      in_section = _wcsicmp(trim_copy(name).c_str(), L"Agent") == 0;
      continue;
    }
    if (!in_section) continue;
    const std::wstring::size_type equals = line.find(L'=');
    if (equals == std::wstring::npos) continue;
    if (_wcsicmp(trim_copy(line.substr(0, equals)).c_str(), key) == 0)
      return trim_copy(line.substr(equals + 1));
  }
  return default_value;
}

bool read_bool(const std::wstring& ini, const wchar_t* key, bool default_value) {
  const std::wstring value = read_ini_string(ini, key, default_value ? L"1" : L"0");
  return value == L"1" || _wcsicmp(value.c_str(), L"true") == 0;
}

DWORD read_ini_int(const std::wstring& ini, const wchar_t* key, int default_value) {
  const std::wstring value = read_ini_string(ini, key, L"");
  if (value.empty()) return static_cast<DWORD>(default_value);
  return static_cast<DWORD>(_wtoi(value.c_str()));
}

// Unique enough for the server's request de-duplication window; the agent has no
// COM initialised in service context, so no CoCreateGuid here.
std::string make_client_request_id() {
  static volatile LONG counter = 0;
  const LONG sequence = InterlockedIncrement(&counter);
  char buffer[64] = {};
  sprintf_s(buffer, sizeof(buffer), "xp-%08lx-%08lx",
            static_cast<unsigned long>(GetTickCount()),
            static_cast<unsigned long>(sequence));
  return std::string(buffer);
}

bool read_small_file(const std::wstring& path, std::string* value) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD size = GetFileSize(file, nullptr);
  if (size == INVALID_FILE_SIZE || size == 0 || size > 4096) {
    CloseHandle(file);
    return false;
  }
  value->assign(size, '\0');
  DWORD read = 0;
  const BOOL success = ReadFile(file, &(*value)[0], size, &read, nullptr);
  CloseHandle(file);
  if (!success || read != size) return false;
  while (!value->empty() && (value->back() == '\r' || value->back() == '\n' ||
                             value->back() == ' ' || value->back() == '\t')) value->pop_back();
  std::size_t start = 0;
  while (start < value->size() && ((*value)[start] == ' ' || (*value)[start] == '\t')) ++start;
  if (start != 0) value->erase(0, start);
  return !value->empty();
}

bool contains_line_break(const std::wstring& value) {
  return value.find(L'\r') != std::wstring::npos || value.find(L'\n') != std::wstring::npos;
}

int configure_agent(
    const std::wstring& token_source,
    const std::wstring& server_url,
    const std::wstring& debug_log) {
  if (contains_line_break(server_url) || (debug_log != L"0" && debug_log != L"1")) {
    std::fwprintf(stderr, L"configure_failed error=\"invalid configuration value\"\n");
    return ERROR_INVALID_PARAMETER;
  }
  const std::wstring common_data = usb_dlp::common_application_data_path();
  const std::wstring data_directory = join_path(common_data, L"UsbDlp");
  std::wstring error;
  if (common_data.empty() ||
      !usb_dlp::ensure_protected_directory(data_directory, &error)) {
    std::fwprintf(stderr, L"configure_failed data_directory=\"%ls\" error=\"%ls\"\n",
                  data_directory.c_str(), error.c_str());
    return ERROR_ACCESS_DENIED;
  }
  const std::wstring token_destination = join_path(data_directory, L"bootstrap-token.txt");
  if (!CopyFileW(token_source.c_str(), token_destination.c_str(), FALSE)) {
    std::fwprintf(stderr, L"configure_failed data_directory=\"%ls\" operation=\"CopyFile(token)\" error=%lu\n",
                  data_directory.c_str(), GetLastError());
    return ERROR_WRITE_FAULT;
  }
  std::wstring ini_contents =
      L"[Agent]\r\n"
      L"DryRun=0\r\n"
      L"DebugLog=" + debug_log + L"\r\n"
      L"ServerUrl=" + server_url + L"\r\n"
      L"AllowInsecureHttp=1\r\n"
      L"EnrollmentTokenFile=" + token_destination + L"\r\n"
      L"LocalPollSeconds=2\r\n"
      L"PolicyPollSeconds=30\r\n";
#ifndef USB_DLP_LEGACY_XP
  // XP has no Device Installation Restrictions layer, so the knob would only
  // advertise an option the binary cannot honour.
  ini_contents += L"Backstop=1\r\n";
#endif
  if (!usb_dlp::write_utf16_text_file(
          join_path(data_directory, L"legacy-agent.ini"), ini_contents, &error)) {
    std::fwprintf(stderr, L"configure_failed data_directory=\"%ls\" error=\"%ls\"\n",
                  data_directory.c_str(), error.c_str());
    return ERROR_WRITE_FAULT;
  }
  std::fwprintf(stdout, L"configure_succeeded data_directory=\"%ls\"\n", data_directory.c_str());
  return 0;
}

DWORD read_poll_ms(const std::wstring& ini) {
  UINT seconds = read_ini_int(ini, L"LocalPollSeconds", 2);
  if (seconds < 1) seconds = 1;
  if (seconds > 300) seconds = 300;
  return seconds * 1000;
}

void report_status(DWORD state, DWORD error, DWORD wait_hint) {
  if (g_status_handle == nullptr) return;
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = error;
  g_status.dwWaitHint = wait_hint;
  g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
  SetServiceStatus(g_status_handle, &g_status);
}

void WINAPI control_handler(DWORD control) {
  if ((control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) && g_stop_event != nullptr) {
    report_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
    SetEvent(g_stop_event);
  }
}

typedef std::map<std::wstring, usb_dlp::WindowsDevice> OwnedDevices;

bool save_owned_devices(
    const usb_dlp::SecureStore& store,
    const OwnedDevices& devices,
    std::wstring* error) {
  usb_dlp::JsonValue root = usb_dlp::JsonValue::array();
  for (const auto& pair : devices) {
    const usb_dlp::WindowsDevice& device = pair.second;
    usb_dlp::JsonValue item = usb_dlp::JsonValue::object();
    item.object_items()["instanceId"] = usb_dlp::JsonValue(usb_dlp::wide_to_utf8(device.instance_id));
    item.object_items()["friendlyName"] = usb_dlp::JsonValue(usb_dlp::wide_to_utf8(device.friendly_name));
    item.object_items()["vid"] = usb_dlp::JsonValue(device.policy_device.vid);
    item.object_items()["pid"] = usb_dlp::JsonValue(device.policy_device.pid);
    item.object_items()["serial"] = usb_dlp::JsonValue(device.policy_device.serial);
    item.object_items()["hasSerial"] = usb_dlp::JsonValue(device.policy_device.has_serial);
    item.object_items()["classGuid"] = usb_dlp::JsonValue(device.policy_device.class_guid);
    item.object_items()["deviceClass"] = usb_dlp::JsonValue(
        device.policy_device.device_class == usb_dlp::ProductDeviceClass::usb_storage ?
            std::string("usb_storage") : std::string("wpd"));
    root.array_items().push_back(item);
  }
  return store.save(L"owned-devices.dat", usb_dlp::write_json(root), error);
}

bool load_owned_devices(
    const usb_dlp::SecureStore& store,
    OwnedDevices* devices,
    std::wstring* error) {
  if (!store.exists(L"owned-devices.dat")) return true;
  std::string encoded;
  if (!store.load(L"owned-devices.dat", &encoded, error)) return false;
  usb_dlp::JsonValue root;
  std::string parse_error;
  if (!usb_dlp::parse_json(encoded, &root, &parse_error) ||
      root.type() != usb_dlp::JsonValue::Type::array) {
    if (error != nullptr) *error = L"Invalid owned-devices state";
    return false;
  }
  OwnedDevices parsed;
  for (const usb_dlp::JsonValue& item : root.array_items()) {
    if (item.type() != usb_dlp::JsonValue::Type::object) return false;
    const usb_dlp::JsonValue* id = item.find("instanceId");
    const usb_dlp::JsonValue* friendly = item.find("friendlyName");
    const usb_dlp::JsonValue* vid = item.find("vid");
    const usb_dlp::JsonValue* pid = item.find("pid");
    const usb_dlp::JsonValue* serial = item.find("serial");
    const usb_dlp::JsonValue* has_serial = item.find("hasSerial");
    const usb_dlp::JsonValue* class_guid = item.find("classGuid");
    const usb_dlp::JsonValue* device_class = item.find("deviceClass");
    if (id == nullptr || friendly == nullptr || vid == nullptr || pid == nullptr ||
        serial == nullptr || has_serial == nullptr || class_guid == nullptr ||
        device_class == nullptr || id->type() != usb_dlp::JsonValue::Type::string ||
        id->string_value().empty() || friendly->type() != usb_dlp::JsonValue::Type::string ||
        vid->type() != usb_dlp::JsonValue::Type::string ||
        pid->type() != usb_dlp::JsonValue::Type::string ||
        serial->type() != usb_dlp::JsonValue::Type::string ||
        has_serial->type() != usb_dlp::JsonValue::Type::boolean ||
        class_guid->type() != usb_dlp::JsonValue::Type::string ||
        device_class->type() != usb_dlp::JsonValue::Type::string ||
        (device_class->string_value() != "usb_storage" && device_class->string_value() != "wpd")) {
      return false;
    }
    usb_dlp::WindowsDevice device;
    device.instance_id = usb_dlp::utf8_to_wide(id->string_value());
    device.friendly_name = usb_dlp::utf8_to_wide(friendly->string_value());
    device.policy_device.instance_id = id->string_value();
    device.policy_device.vid = vid->string_value();
    device.policy_device.pid = pid->string_value();
    device.policy_device.serial = serial->string_value();
    device.policy_device.has_serial = has_serial->boolean_value();
    device.policy_device.class_guid = class_guid->string_value();
    device.policy_device.device_class = device_class->string_value() == "usb_storage" ?
        usb_dlp::ProductDeviceClass::usb_storage : usb_dlp::ProductDeviceClass::wpd;
    parsed[device.instance_id] = device;
  }
  devices->swap(parsed);
  return true;
}

int restore_owned_devices() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) return static_cast<int>(GetLastError());
  SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS | SERVICE_STOP);
  if (service != nullptr) {
    const DWORD started = GetTickCount();
    SERVICE_STATUS status = {};
    while (QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED) {
      if (status.dwCurrentState == SERVICE_RUNNING) {
        SERVICE_STATUS ignored = {};
        ControlService(service, SERVICE_CONTROL_STOP, &ignored);
      }
      if (static_cast<DWORD>(GetTickCount() - started) >= 30000) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        std::fwprintf(stderr, L"restore_owned_failed error=service_stop_timeout\n");
        return ERROR_TIMEOUT;
      }
      Sleep(250);
    }
    if (status.dwCurrentState != SERVICE_STOPPED) {
      const DWORD result = GetLastError();
      CloseServiceHandle(service);
      CloseServiceHandle(manager);
      return static_cast<int>(result);
    }
    CloseServiceHandle(service);
  } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
    const DWORD result = GetLastError();
    CloseServiceHandle(manager);
    return static_cast<int>(result);
  }
  CloseServiceHandle(manager);
  const std::wstring common_data = usb_dlp::common_application_data_path();
  if (common_data.empty()) return ERROR_PATH_NOT_FOUND;
  const std::wstring data_directory = join_path(common_data, L"UsbDlp");
  usb_dlp::SecureStore store(data_directory);
  OwnedDevices owned;
  std::wstring error;
  if (!load_owned_devices(store, &owned, &error)) {
    std::fwprintf(stderr, L"restore_owned_failed error=\"%ls\"\n", error.c_str());
    return ERROR_INVALID_DATA;
  }
  usb_dlp::WindowsDeviceEnforcer enforcer;
  for (OwnedDevices::iterator it = owned.begin(); it != owned.end();) {
    bool disabled = false;
    unsigned long result = 0;
    const bool status_known = enforcer.is_disabled(it->first, &disabled, &result);
    if (!status_known) {
      std::fwprintf(stderr, L"restore_owned_pending instance_id=\"%ls\" result=%lu reconnect_device=true\n",
                    it->first.c_str(), result);
      ++it;
      continue;
    }
    if (disabled && !enforcer.enable(it->first, &result)) {
      std::fwprintf(stderr, L"restore_owned_failed instance_id=\"%ls\" result=%lu\n",
                    it->first.c_str(), result);
      ++it;
      continue;
    }
    std::fwprintf(stdout, L"restore_owned_succeeded instance_id=\"%ls\"\n", it->first.c_str());
    it = owned.erase(it);
  }
  if (!save_owned_devices(store, owned, &error)) {
    std::fwprintf(stderr, L"restore_owned_save_failed error=\"%ls\"\n", error.c_str());
    return ERROR_WRITE_FAULT;
  }
  return owned.empty() ? 0 : ERROR_DEVICE_NOT_CONNECTED;
}

// ---------------------------------------------------------------------------
// Offline approval
// ---------------------------------------------------------------------------

// A disabled device stays in the enumeration store but drops out of a
// present-only scan, so the agent loses sight of devices it blocked itself.
// CM_Locate_DevNode still finds it, which is what separates "blocked and still
// plugged in" from "unplugged".
bool device_is_present(const std::wstring& instance_id) {
  DEVINST node = 0;
  if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instance_id.c_str()),
                         CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
    if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instance_id.c_str()),
                           CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS) {
      return false;
    }
  }
  ULONG status = 0;
  ULONG problem = 0;
  return CM_Get_DevNode_Status(&status, &problem, node, 0) == CR_SUCCESS;
}

// A WPD mirror node's instance id embeds its parent disk id:
//   SWD\WPDBUSENUM\_??_USBSTOR#DISK&VEN_X#123&0#{GUID}
// must map to
//   USBSTOR\DISK&VEN_X\123&0
// Approving only the disk left the mirror blocked, so the device still worked
// while the prompt kept claiming it was blocked.
bool is_wpd_mirror(const std::wstring& instance_id) {
  return instance_id.find(L"WPDBUSENUM") != std::wstring::npos;
}

std::wstring wpd_parent_instance_id(const std::wstring& instance_id) {
  const std::wstring marker = L"_??_";
  const std::size_t marker_at = instance_id.find(marker);
  if (marker_at == std::wstring::npos) return std::wstring();
  std::wstring rest = instance_id.substr(marker_at + marker.size());
  const std::size_t guid_at = rest.find(L"#{");
  if (guid_at != std::wstring::npos) rest.resize(guid_at);
  const std::size_t first = rest.find(L'#');
  if (first == std::wstring::npos) return std::wstring();
  const std::size_t last = rest.rfind(L'#');
  if (last == first) return std::wstring();
  rest[first] = L'\\';
  rest[last] = L'\\';
  return rest;
}

// True when the policy holds a live allow entry for exactly this instance.
bool policy_allows_instance(const usb_dlp::CompiledPolicy& policy, const std::wstring& instance_id,
                            std::int64_t now_utc_ms) {
  const std::string target = usb_dlp::wide_to_utf8(instance_id);
  for (const usb_dlp::DeviceEntry& entry : policy.entries) {
    if (entry.action != usb_dlp::EntryAction::allow) continue;
    if (entry.match_type != usb_dlp::MatchType::instance_id) continue;
    if (entry.instance_id != target) continue;
    if (entry.has_expiry && entry.expires_at_utc_ms <= now_utc_ms) continue;
    return true;
  }
  return false;
}

// Offline grants are kept apart from policy.dat, which stores the verbatim
// payload pushed by the server. Mixing the two would make the next policy poll
// silently discard every locally approved device.
std::vector<usb_dlp::DeviceEntry> parse_approval_entries(const std::string& text) {
  std::vector<usb_dlp::DeviceEntry> entries;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line =
        text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? text.size() : end + 1;
    if (line.empty()) continue;
    const std::size_t first = line.find('\t');
    if (first == std::string::npos) continue;
    const std::size_t second = line.find('\t', first + 1);
    usb_dlp::DeviceEntry entry;
    entry.instance_id = line.substr(0, first);
    entry.expires_at_utc_ms = std::strtoll(line.c_str() + first + 1, nullptr, 10);
    entry.id = second == std::string::npos ? std::string() : line.substr(second + 1);
    if (entry.id.empty()) entry.id = "offline";
    entry.match_type = usb_dlp::MatchType::instance_id;
    entry.action = usb_dlp::EntryAction::allow;
    entry.scope_type = usb_dlp::ScopeType::global;
    entry.has_expiry = true;
    if (!entry.instance_id.empty()) entries.push_back(entry);
  }
  return entries;
}

// Replay protection has to outlive the process, otherwise a used code becomes
// valid again the moment the service restarts. Entries carry their own expiry
// so the file cannot grow without bound.
void load_used_codes(usb_dlp::SecureStore& store, std::int64_t now_ms,
                     std::map<std::string, std::int64_t>* codes) {
  std::string text;
  std::wstring error;
  if (!store.load(L"used-codes.dat", &text, &error)) return;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line =
        text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? text.size() : end + 1;
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    const std::int64_t expires = std::strtoll(line.c_str() + tab + 1, nullptr, 10);
    if (expires <= now_ms) continue;
    (*codes)[line.substr(0, tab)] = expires;
  }
}

std::string serialize_used_codes(const std::map<std::string, std::int64_t>& codes,
                                 std::int64_t now_ms) {
  std::string text;
  for (std::map<std::string, std::int64_t>::const_iterator it = codes.begin();
       it != codes.end(); ++it) {
    if (it->second <= now_ms) continue;
    text += it->first;
    text += '\t';
    text += std::to_string(it->second);
    text += '\n';
  }
  return text;
}

std::string serialize_approval_entries(const std::vector<usb_dlp::DeviceEntry>& entries) {
  std::string text;
  for (const usb_dlp::DeviceEntry& entry : entries) {
    text += entry.instance_id;
    text += '\t';
    text += std::to_string(entry.expires_at_utc_ms);
    text += '\t';
    text += entry.id;
    text += '\n';
  }
  return text;
}

// Monotonic wall clock. An approval code carries its own validity window, so
// the endpoint has to decide "has this expired" against a time the user cannot
// simply set backwards. The high-water mark is persisted, otherwise a reboot
// would reset it and a clock rollback could revive an expired grant.
class TrustedClock {
 public:
  TrustedClock() : high_water_ms_(0), last_saved_ms_(0) {
    InitializeCriticalSection(&lock_);
  }
  ~TrustedClock() { DeleteCriticalSection(&lock_); }

  TrustedClock(const TrustedClock&) = delete;
  TrustedClock& operator=(const TrustedClock&) = delete;

  void initialize(const std::wstring& data_directory) {
    data_directory_ = data_directory;
    usb_dlp::SecureStore store(data_directory);
    std::string stored;
    std::wstring error;
    if (!store.load(L"clock.dat", &stored, &error)) return;
    char* end = nullptr;
    const long long value = std::strtoll(stored.c_str(), &end, 10);
    if (end == nullptr || end == stored.c_str() || value <= 0) return;
    high_water_ms_ = static_cast<std::int64_t>(value);
    last_saved_ms_ = high_water_ms_;
  }

  // Feeds a raw system time and returns the instant policy should be judged
  // against: never earlier than anything already observed.
  std::int64_t apply(std::int64_t raw_ms, bool* rolled_back, std::int64_t* previous_ms) {
    bool changed = false;
    std::int64_t result = raw_ms;
    EnterCriticalSection(&lock_);
    if (raw_ms + 2000 < high_water_ms_) {
      if (rolled_back != nullptr) *rolled_back = true;
      if (previous_ms != nullptr) *previous_ms = high_water_ms_;
      result = high_water_ms_;
    } else {
      if (rolled_back != nullptr) *rolled_back = false;
      if (raw_ms > high_water_ms_) {
        high_water_ms_ = raw_ms;
        changed = true;
      }
      result = high_water_ms_;
    }
    const bool should_save = changed && (high_water_ms_ - last_saved_ms_ >= 60000);
    const std::int64_t value_to_save = high_water_ms_;
    LeaveCriticalSection(&lock_);

    if (should_save) {
      usb_dlp::SecureStore store(data_directory_);
      std::wstring error;
      if (store.save(L"clock.dat", std::to_string(value_to_save), &error)) {
        EnterCriticalSection(&lock_);
        last_saved_ms_ = value_to_save;
        LeaveCriticalSection(&lock_);
      }
    }
    return result;
  }

  // Current trusted instant, for code validation outside the poll loop.
  std::int64_t now_ms() const {
    FILETIME file_time = {};
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER ticks = {};
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    const std::int64_t raw_ms =
        static_cast<std::int64_t>((ticks.QuadPart - 116444736000000000ULL) / 10000ULL);
    EnterCriticalSection(&lock_);
    const std::int64_t value = high_water_ms_ > raw_ms ? high_water_ms_ : raw_ms;
    LeaveCriticalSection(&lock_);
    return value;
  }

 private:
  mutable CRITICAL_SECTION lock_;
  std::wstring data_directory_;
  std::int64_t high_water_ms_;
  std::int64_t last_saved_ms_;
};

struct ApprovalState {
  usb_dlp::SecureStore* store = nullptr;
  TrustedClock* clock = nullptr;
  usb_dlp::WindowsDeviceEnforcer* enforcer = nullptr;
  std::wstring data_directory;
  CRITICAL_SECTION lock = {};
  std::vector<usb_dlp::DeviceEntry> pending;
  std::map<std::string, std::int64_t> used_codes;
  std::vector<std::uint8_t> short_key;
  bool short_code_enabled = true;
};

// Loads the per-device short-code key, creating it on first run. Each endpoint
// gets its own key so that extracting one does not compromise the fleet.
bool load_short_code_key(const std::wstring& data_directory, std::vector<std::uint8_t>* key,
                         std::wstring* error) {
  usb_dlp::SecureStore store(data_directory);
  std::string stored;
  if (store.load(L"short-key.dat", &stored, error)) {
    std::vector<std::uint8_t> decoded;
    if (usb_dlp::crypto::base32_decode(stored, &decoded) && decoded.size() == 32) {
      *key = decoded;
      return true;
    }
    log_line(data_directory, L"short_key_invalid regenerating=true");
  }

  std::vector<std::uint8_t> fresh(32);
  HCRYPTPROV provider = 0;
  bool generated = false;
  if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
    generated = CryptGenRandom(provider, static_cast<DWORD>(fresh.size()), &fresh[0]) != FALSE;
    CryptReleaseContext(provider, 0);
  }
  if (!generated) {
    if (error != nullptr) *error = L"Could not generate a short-code key.";
    return false;
  }
  const std::string encoded = usb_dlp::crypto::base32_encode(&fresh[0], fresh.size());
  if (!store.save(L"short-key.dat", encoded, error)) return false;
  *key = fresh;
  log_line(data_directory, L"short_key_created");
  return true;
}

// Runs on the notifier pipe thread. It may only append to `pending`; the poll
// loop owns the live policy and applies grants on its next iteration.
bool handle_approval_code(void* context, const std::wstring& instance_id,
                          const std::string& code, std::string* message) {
  ApprovalState* state = static_cast<ApprovalState*>(context);
  const std::string instance_utf8 = usb_dlp::wide_to_utf8(instance_id);
  const std::int64_t now_ms = state->clock->now_ms();

  std::vector<std::uint8_t> decoded;
  if (!usb_dlp::crypto::base32_decode(code, &decoded)) {
    *message = "invalid_format";
    // Log enough to tell a bad character apart from a truncated paste, without
    // ever writing the code itself.
    std::string sample;
    for (std::size_t index = 0; index < code.size() && index < 8; ++index) {
      const unsigned char value = static_cast<unsigned char>(code[index]);
      sample.push_back(value >= 0x20 && value < 0x7F ? static_cast<char>(value) : '?');
    }
    log_line(state->data_directory,
             L"approval_decode_failed length=" + std::to_wstring(code.size()) +
                 L" head=" + usb_dlp::utf8_to_wide(sample));
    return false;
  }

  usb_dlp::ApprovalGrant grant;
  usb_dlp::ApprovalError error = usb_dlp::ApprovalError::none;
  bool accepted = false;
  if (decoded.size() == usb_dlp::kSignedCodeBytes) {
    const std::uint8_t* bytes = usb_dlp::approval_modulus();
    const std::vector<std::uint8_t> modulus(bytes, bytes + 256);
    accepted = usb_dlp::verify_signed_approval_code(code, modulus, instance_utf8, now_ms, &grant,
                                                    &error);
  } else if (decoded.size() == 15) {
    if (!state->short_code_enabled) {
      *message = "short_code_disabled";
      return false;
    }
    if (state->short_key.empty()) {
      *message = "short_key_missing";
      return false;
    }
    accepted = usb_dlp::verify_short_approval_code(code, state->short_key, instance_utf8, now_ms,
                                                   &grant, &error);
  } else {
    *message = "invalid_format";
    log_line(state->data_directory,
             L"approval_bad_length decoded_bytes=" + std::to_wstring(decoded.size()));
    return false;
  }

  if (!accepted) {
    *message = usb_dlp::approval_error_name(error);
    log_line(state->data_directory,
             L"approval_rejected instance_id=" + instance_id + L" result=" +
                 usb_dlp::utf8_to_wide(usb_dlp::approval_error_name(error)) +
                 L" code=" + usb_dlp::utf8_to_wide(usb_dlp::approval_code_fingerprint(code).substr(0, 16)));
    return false;
  }

  const std::string fingerprint = usb_dlp::approval_code_fingerprint(code);
  bool replay = false;
  std::string used_codes_text;
  EnterCriticalSection(&state->lock);
  std::map<std::string, std::int64_t>::const_iterator seen = state->used_codes.find(fingerprint);
  replay = seen != state->used_codes.end() && seen->second > now_ms;
  if (!replay) {
    usb_dlp::DeviceEntry entry;
    entry.id = "offline-" + fingerprint.substr(0, 16);
    entry.match_type = usb_dlp::MatchType::instance_id;
    entry.instance_id = instance_utf8;
    entry.action = usb_dlp::EntryAction::allow;
    entry.scope_type = usb_dlp::ScopeType::global;
    entry.has_expiry = true;
    entry.expires_at_utc_ms = grant.not_after_utc_ms;
    state->pending.push_back(entry);
    state->used_codes[fingerprint] = grant.not_after_utc_ms;
    used_codes_text = serialize_used_codes(state->used_codes, now_ms);
  }
  LeaveCriticalSection(&state->lock);

  if (!used_codes_text.empty() && state->store != nullptr) {
    std::wstring save_error;
    if (!state->store->save(L"used-codes.dat", used_codes_text, &save_error)) {
      log_line(state->data_directory, L"used_codes_save_error " + save_error);
    }
  }

  if (replay) {
    *message = "replay";
    return false;
  }

  *message = grant.signed_code ? "ok_signed" : "ok_short";
  log_line(state->data_directory,
           L"approval_accepted instance_id=" + instance_id +
               L" kind=" + std::wstring(grant.signed_code ? L"signed" : L"short") +
               L" duration_h=" + std::to_wstring(grant.duration_hours) +
               L" code=" + usb_dlp::utf8_to_wide(fingerprint.substr(0, 16)));

  // A disabled device is gone from the enumeration, so the polling loop can
  // never see it again on its own. Re-enable the node directly, otherwise the
  // grant would only take effect after the user unplugged and replugged.
  if (state->enforcer != nullptr) {
    unsigned long enable_result = 0;
    if (state->enforcer->enable(instance_id, &enable_result)) {
      log_line(state->data_directory, L"approval_devnode_enabled instance_id=" + instance_id);
    } else {
      log_line(state->data_directory,
               L"approval_devnode_enable_failed instance_id=" + instance_id +
                   L" result=" + std::to_wstring(enable_result));
      *message = grant.signed_code ? "ok_replug_signed" : "ok_replug_short";
    }
  }
  return true;
}

int run_agent(HANDLE stop_event) {
  const std::wstring common_data = usb_dlp::common_application_data_path();
  if (common_data.empty()) return ERROR_PATH_NOT_FOUND;
  const std::wstring data_directory = join_path(common_data, L"UsbDlp");
  std::wstring storage_error;
  if (!usb_dlp::ensure_protected_directory(data_directory, &storage_error)) return ERROR_ACCESS_DENIED;
  const std::wstring ini = join_path(data_directory, L"legacy-agent.ini");
  const bool dry_run = read_bool(ini, L"DryRun", true);
  const bool debug_log = read_bool(ini, L"DebugLog", false);
  const DWORD poll_ms = read_poll_ms(ini);
  UINT policy_poll_seconds = read_ini_int(ini, L"PolicyPollSeconds", 30);
  if (policy_poll_seconds < 5) policy_poll_seconds = 5;
  if (policy_poll_seconds > 3600) policy_poll_seconds = 3600;
  const DWORD policy_poll_ms = policy_poll_seconds * 1000;
  const std::wstring server_url = read_ini_string(ini, L"ServerUrl", L"");
  const bool allow_insecure_http = read_bool(ini, L"AllowInsecureHttp", false);
  const std::wstring token_path = read_ini_string(
      ini, L"EnrollmentTokenFile", join_path(data_directory, L"bootstrap-token.txt").c_str());

  usb_dlp::CompiledPolicy policy;
  policy.schema_version = 1;
  policy.policy_version = 0;
  policy.group_mode = usb_dlp::GroupMode::default_deny;
  usb_dlp::SecureStore secure_store(data_directory);
  usb_dlp::AgentIdentity identity;
  bool has_identity = false;
  if (secure_store.exists(L"identity.dat")) {
    std::string identity_json;
    has_identity = secure_store.load(L"identity.dat", &identity_json, &storage_error) &&
                   usb_dlp::decode_identity(identity_json, &identity);
  }
  if (secure_store.exists(L"policy.dat")) {
    std::string policy_json;
    std::string policy_error;
    if (secure_store.load(L"policy.dat", &policy_json, &storage_error) &&
        usb_dlp::decode_compiled_policy(policy_json, &policy, &policy_error)) {
      log_line(data_directory, L"policy_loaded version=" + std::to_wstring(policy.policy_version));
    } else {
      policy = usb_dlp::CompiledPolicy();
      policy.group_mode = usb_dlp::GroupMode::default_deny;
      log_line(data_directory, L"policy_invalid fail_closed=true");
    }
  }
  usb_dlp::WindowsDeviceEnumerator enumerator;
  usb_dlp::WindowsDeviceEnforcer enforcer;
  usb_dlp::LegacyAgentApi api(server_url, allow_insecure_http);
  usb_dlp::NotifierBridge notifier(
      &api, join_path(module_directory(), L"UsbDlp.LegacyNotifier.exe"));
  if (has_identity) notifier.set_identity(identity);
  if (!notifier.start(&storage_error)) {
    log_line(data_directory, L"notifier_pipe_error " + storage_error);
  }

  // Offline approval: trusted clock, per-device short-code key, grant channel.
  TrustedClock trusted_clock;
  trusted_clock.initialize(data_directory);
  {
    std::string approvals_text;
    if (secure_store.load(L"approvals.dat", &approvals_text, &storage_error)) {
      const std::vector<usb_dlp::DeviceEntry> restored = parse_approval_entries(approvals_text);
      for (const usb_dlp::DeviceEntry& entry : restored) policy.entries.push_back(entry);
      if (!restored.empty()) {
        log_line(data_directory, L"approvals_restored count=" + std::to_wstring(restored.size()));
      }
    }
  }
  ApprovalState approval_state;
  approval_state.store = &secure_store;
  approval_state.clock = &trusted_clock;
  approval_state.enforcer = &enforcer;
  approval_state.data_directory = data_directory;
  approval_state.short_code_enabled = read_bool(ini, L"ShortCode", true);
  InitializeCriticalSection(&approval_state.lock);
  load_used_codes(secure_store, trusted_clock.now_ms(), &approval_state.used_codes);
  if (approval_state.short_code_enabled) {
    std::wstring key_error;
    std::vector<std::uint8_t> key;
    if (load_short_code_key(data_directory, &key, &key_error)) {
      approval_state.short_key = key;
      log_line(data_directory,
               L"short_code_ready key=" +
                   usb_dlp::utf8_to_wide(usb_dlp::crypto::base32_encode(&key[0], key.size())));
    } else {
      log_line(data_directory, L"short_key_error " + key_error);
    }
  } else {
    log_line(data_directory, L"short_code_disabled_by_config");
  }
  notifier.set_approval_handler(handle_approval_code, &approval_state);
  OwnedDevices owned_devices;
  if (!load_owned_devices(secure_store, &owned_devices, &storage_error)) {
    log_line(data_directory, L"owned_devices_load_error " + storage_error);
  }
  std::set<std::wstring> unverified_nodes;
  std::set<std::wstring> attempted_nodes;
  std::set<std::wstring> requested_nodes;
  std::set<std::wstring> previous_present_nodes;
  std::set<std::wstring> previous_device_diagnostics;
#ifndef USB_DLP_LEGACY_XP
  const bool backstop_enabled = read_bool(ini, L"Backstop", true);
  usb_dlp::RegistryBackstop backstop;
  std::wstring backstop_error;
  if (backstop_enabled && !backstop.initialize(data_directory, &backstop_error)) {
    log_line(data_directory, L"backstop_init_error " + backstop_error);
  }
  std::int64_t backstop_policy_version = -1;
  bool backstop_conflict_logged = false;
#endif
  DWORD last_policy_poll = GetTickCount() - policy_poll_ms;
  log_line(data_directory,
           (dry_run ? std::wstring(L"agent_start build=") : std::wstring(L"agent_start build=")) +
               usb_dlp::utf8_to_wide(USB_DLP_BUILD_STAMP) +
               (dry_run ? L" dry_run=true" : L" dry_run=false default_deny=true"));
#ifdef USB_DLP_XP_SETUPAPI
  log_line(data_directory, L"enforcement_backend=setupapi_propertychange");
#elif defined(USB_DLP_XP_FILTER)
  log_line(data_directory, L"enforcement_backend=xp_disk_filter");
#else
  log_line(data_directory, L"enforcement_backend=config_manager");
#endif
  if (debug_log) {
    log_line(data_directory, L"debug_config enabled=true local_poll_ms=" + std::to_wstring(poll_ms) +
                                 L" policy_poll_ms=" + std::to_wstring(policy_poll_ms) +
                                 L" server_configured=" + (server_url.empty() ? L"false" : L"true") +
                                 L" identity_cached=" + (has_identity ? L"true" : L"false"));
  }

  for (;;) {
    const DWORD now_ticks = GetTickCount();
    if (api.valid() && static_cast<DWORD>(now_ticks - last_policy_poll) >= policy_poll_ms) {
      last_policy_poll = now_ticks;
      if (!has_identity) {
        std::string token;
        const bool token_loaded = read_small_file(token_path, &token);
        storage_error.clear();
        if (token_loaded && api.enroll(token, USB_DLP_PROFILE, &identity, &storage_error)) {
          std::string encoded;
          if (usb_dlp::encode_identity(identity, &encoded) &&
              secure_store.save(L"identity.dat", encoded, &storage_error)) {
            has_identity = true;
            notifier.set_identity(identity);
            DeleteFileW(token_path.c_str());
            log_line(data_directory, L"enrollment_succeeded");
          }
        } else if (debug_log) {
          log_line(data_directory, L"debug_enrollment_pending error=" +
                                       (!token_loaded ? L"token_unavailable_or_invalid"
                                                      : (storage_error.empty() ? L"unknown"
                                                                               : storage_error)));
        }
        if (!token.empty()) SecureZeroMemory(&token[0], token.size());
      }
      if (has_identity) {
        usb_dlp::CompiledPolicy downloaded;
        std::string raw_policy;
        if (api.fetch_policy(identity, &downloaded, &raw_policy, &storage_error) &&
            secure_store.save(L"policy.dat", raw_policy, &storage_error)) {
          policy = downloaded;
          log_line(data_directory, L"policy_applied version=" + std::to_wstring(policy.policy_version) +
                                       L" mode=" + group_mode_name(policy.group_mode) +
                                       L" entries=" + std::to_wstring(policy.entries.size()));
        } else if (debug_log) {
          log_line(data_directory, L"debug_policy_fetch_failed error=" +
                                       (storage_error.empty() ? L"unknown" : storage_error));
        }
        if (!api.heartbeat(identity, policy.policy_version, 0, &storage_error) && debug_log) {
          log_line(data_directory, L"debug_heartbeat_failed error=" +
                                       (storage_error.empty() ? L"unknown" : storage_error));
        }
      }
    }

#ifndef USB_DLP_LEGACY_XP
    // Publish the passive baseline whenever the effective policy changes. It is
    // deliberately NOT reverted when this loop exits: the whole point is that it
    // outlives the service. Only an explicit --restore-backstop, run by the
    // uninstall path, removes it.
    if (backstop_enabled && backstop_policy_version != policy.policy_version) {
      FILETIME backstop_file_time = {};
      GetSystemTimeAsFileTime(&backstop_file_time);
      ULARGE_INTEGER backstop_ticks = {};
      backstop_ticks.LowPart = backstop_file_time.dwLowDateTime;
      backstop_ticks.HighPart = backstop_file_time.dwHighDateTime;
      const std::int64_t backstop_now_ms = static_cast<std::int64_t>(
          (backstop_ticks.QuadPart - 116444736000000000ULL) / 10000ULL);
      std::vector<std::wstring> allow_ids;
      std::vector<std::wstring> deny_ids;
      for (const usb_dlp::DeviceEntry& entry : policy.entries) {
        if (entry.has_expiry && entry.expires_at_utc_ms <= backstop_now_ms) continue;
        if (entry.instance_id.empty()) continue;
        // Only instance IDs are published. Hardware and compatible IDs are model
        // level and would approve every unit of that model, including devices the
        // administrator never saw.
        if (entry.match_type != usb_dlp::MatchType::instance_id) continue;
        if (entry.action == usb_dlp::EntryAction::allow) {
          allow_ids.push_back(usb_dlp::utf8_to_wide(entry.instance_id));
        } else {
          deny_ids.push_back(usb_dlp::utf8_to_wide(entry.instance_id));
        }
      }
      const bool default_deny_mode = policy.group_mode == usb_dlp::GroupMode::default_deny;
      const usb_dlp::BackstopResult backstop_result =
          backstop.apply(default_deny_mode, allow_ids, deny_ids, dry_run);
      backstop_policy_version = policy.policy_version;
      const std::wstring mode_name =
          default_deny_mode ? std::wstring(L"default_deny") : std::wstring(L"default_allow");
      if (backstop_result.conflict) {
        if (!backstop_conflict_logged) {
          log_line(data_directory,
                   L"external_policy_conflict component=device_install_restrictions detail=" +
                       backstop_result.detail + L" active_enforcement_continues=true");
          backstop_conflict_logged = true;
        }
      } else if (backstop_result.applied) {
        backstop_conflict_logged = false;
        log_line(data_directory, L"backstop_applied mode=" + mode_name +
                                     L" allow=" + std::to_wstring(backstop_result.allow_values) +
                                     L" expanded=" + std::to_wstring(backstop_result.expanded_ids) +
                                     L" deny=" + std::to_wstring(deny_ids.size()));
      } else if (backstop_result.unchanged) {
        log_line(data_directory, L"backstop_unchanged mode=" + mode_name +
                                     L" allow=" + std::to_wstring(backstop_result.allow_values));
      } else {
        log_line(data_directory, L"backstop_skipped detail=" + backstop_result.detail);
      }
    }
#endif

    std::vector<usb_dlp::WindowsDevice> devices;
    std::vector<std::wstring> device_diagnostics;
    std::wstring error;
    if (!enumerator.enumerate(&devices, &error, debug_log ? &device_diagnostics : nullptr)) {
      log_line(data_directory, L"enumeration_error " + error);
    } else {
      if (debug_log) {
        const std::set<std::wstring> current_device_diagnostics(
            device_diagnostics.begin(), device_diagnostics.end());
        for (const std::wstring& diagnostic : current_device_diagnostics) {
          if (previous_device_diagnostics.find(diagnostic) == previous_device_diagnostics.end()) {
            log_line(data_directory, L"debug_enumeration " + diagnostic);
          }
        }
        for (const std::wstring& diagnostic : previous_device_diagnostics) {
          if (current_device_diagnostics.find(diagnostic) == current_device_diagnostics.end()) {
            log_line(data_directory, L"debug_enumeration_removed " + diagnostic);
          }
        }
        previous_device_diagnostics = current_device_diagnostics;
      }
      std::set<std::wstring> present_nodes;
      // Apply grants the approval handler queued on the notifier thread. The
      // policy object stays owned by this loop, so the handler only appends.
      if (!approval_state.pending.empty()) {
        std::vector<usb_dlp::DeviceEntry> applied;
        EnterCriticalSection(&approval_state.lock);
        applied.swap(approval_state.pending);
        LeaveCriticalSection(&approval_state.lock);
        bool changed = false;
        for (const usb_dlp::DeviceEntry& entry : applied) {
          bool duplicate = false;
          for (const usb_dlp::DeviceEntry& existing : policy.entries) {
            if (existing.instance_id == entry.instance_id && existing.action == entry.action) {
              duplicate = true;
              break;
            }
          }
          if (duplicate) continue;
          policy.entries.push_back(entry);
          policy.policy_version += 1;
          changed = true;
          log_line(data_directory, L"approval_entry_added instance_id=" +
                                       usb_dlp::utf8_to_wide(entry.instance_id) +
                                       L" expires_utc_ms=" +
                                       std::to_wstring(entry.expires_at_utc_ms));
        }
        if (changed) {
          std::string approvals_text;
          std::wstring reload_error;
          if (secure_store.load(L"approvals.dat", &approvals_text, &reload_error)) {
            // keep what was there
          } else {
            approvals_text.clear();
          }
          const std::vector<usb_dlp::DeviceEntry> merged =
              parse_approval_entries(approvals_text);
          std::vector<usb_dlp::DeviceEntry> combined = merged;
          for (const usb_dlp::DeviceEntry& entry : applied) combined.push_back(entry);
          if (!secure_store.save(L"approvals.dat", serialize_approval_entries(combined),
                                 &reload_error)) {
            log_line(data_directory, L"approvals_save_error " + reload_error);
          }
        }
      }
      FILETIME file_time = {};
      GetSystemTimeAsFileTime(&file_time);
      ULARGE_INTEGER ticks = {};
      ticks.LowPart = file_time.dwLowDateTime;
      ticks.HighPart = file_time.dwHighDateTime;
      std::int64_t unix_ms = static_cast<std::int64_t>((ticks.QuadPart - 116444736000000000ULL) / 10000ULL);
      {
        // Moving the system clock backwards must not revive an expired grant.
        // The high-water mark now lives on disk, so a reboot cannot reset it.
        static std::int64_t clock_rollback_logged_ms = 0;
        bool rolled_back = false;
        std::int64_t previous_ms = 0;
        const std::int64_t trusted_ms = trusted_clock.apply(unix_ms, &rolled_back, &previous_ms);
        if (rolled_back && previous_ms - unix_ms > 60000 &&
            trusted_ms - clock_rollback_logged_ms > 300000) {
          log_line(data_directory, L"clock_rollback_detected enforced_utc_ms=" +
                                       std::to_wstring(trusted_ms) +
                                       L" raw_utc_ms=" + std::to_wstring(unix_ms));
          clock_rollback_logged_ms = trusted_ms;
        }
        unix_ms = trusted_ms;
      }
      for (const usb_dlp::WindowsDevice& device : devices) {
        present_nodes.insert(device.instance_id);
        const bool newly_present =
            previous_present_nodes.find(device.instance_id) == previous_present_nodes.end();
        usb_dlp::PolicyError policy_error("");
        usb_dlp::PolicyEvaluation evaluation =
            usb_dlp::evaluate_policy(device.policy_device, policy, unix_ms, &policy_error);
        // The WPD mirror of an approved disk must not be blocked on its own.
        if (evaluation.decision == usb_dlp::Decision::block && is_wpd_mirror(device.instance_id)) {
          const std::wstring parent = wpd_parent_instance_id(device.instance_id);
          if (!parent.empty() && policy_allows_instance(policy, parent, unix_ms)) {
            evaluation.decision = usb_dlp::Decision::allow;
            evaluation.reason_code = "wpd_parent_allowed";
          }
        }
        if (newly_present) {
          log_line(data_directory, L"device_seen instance_id=" + device.instance_id +
                                       L" class=" + device_class_name(device.policy_device.device_class) +
                                       L" vid=" + diagnostic_value(device.policy_device.vid) +
                                       L" pid=" + diagnostic_value(device.policy_device.pid) +
                                       L" decision=" +
                                           (evaluation.decision == usb_dlp::Decision::block
                                                ? std::wstring(L"block")
                                                : std::wstring(L"allow")) +
                                       L" reason=" + diagnostic_value(evaluation.reason_code));
        }
        if (evaluation.decision == usb_dlp::Decision::block) {
          bool disabled = false;
          unsigned long result = 0;
          unsigned long status_bits = 0;
          unsigned long problem_code = 0;
          std::wstring enforcement_diagnostic;
          const bool status_known = enforcer.is_disabled(
              device.instance_id, &disabled, &result, &status_bits, &problem_code,
              &enforcement_diagnostic);
          bool attempted_disable = false;
          if (debug_log && newly_present) {
            log_line(data_directory, L"debug_enforcement_status instance_id=" + device.instance_id +
                                         L" known=" + (status_known ? L"true" : L"false") +
                                         L" disabled=" + (disabled ? L"true" : L"false") +
                                         L" result=" + std::to_wstring(result) +
                                         L" stage=" + enforcement_diagnostic +
                                         L" status=" + std::to_wstring(status_bits) +
                                         L" problem=" + std::to_wstring(problem_code));
          }
          if (!dry_run && (!status_known || !disabled)) {
            attempted_disable = true;
            attempted_nodes.insert(device.instance_id);
            if (debug_log) {
              log_line(data_directory, L"debug_disable_attempt instance_id=" + device.instance_id);
            }
            if (!enforcer.disable(device.instance_id, &result, &enforcement_diagnostic)) {
              log_line(data_directory, L"enforce_error instance_id=" + device.instance_id +
                                           L" result=" + std::to_wstring(result) +
                                           L" stage=" + enforcement_diagnostic);
              // CR_NO_SUCH_DEVNODE (13) happens while the USB stack is still
              // rebuilding the node right after a re-plug. Retrying inside the
              // same pass closes most of the window in which the volume is
              // mountable but not yet blocked.
              for (int retry = 0; retry < 5; ++retry) {
                Sleep(100);
                if (enforcer.disable(device.instance_id, &result, &enforcement_diagnostic)) break;
              }
            }
            status_bits = 0;
            problem_code = 0;
            const bool verify_known = enforcer.is_disabled(
                device.instance_id, &disabled, &result, &status_bits, &problem_code,
                &enforcement_diagnostic);
            disabled = verify_known && disabled;
            if (debug_log) {
              log_line(data_directory, L"debug_disable_verify instance_id=" + device.instance_id +
                                           L" known=" + (verify_known ? L"true" : L"false") +
                                           L" disabled=" + (disabled ? L"true" : L"false") +
                                           L" result=" + std::to_wstring(result) +
                                           L" stage=" + enforcement_diagnostic +
                                           L" status=" + std::to_wstring(status_bits) +
                                           L" problem=" + std::to_wstring(problem_code));
            }
          } else if (dry_run && newly_present) {
            log_line(data_directory, L"would_block instance_id=" + device.instance_id);
          } else if (disabled && newly_present) {
            log_line(data_directory, L"already_blocked instance_id=" + device.instance_id);
          }
          if (!dry_run && disabled) {
            if (!filter_usb_disk(device.instance_id) &&
                (attempted_disable || attempted_nodes.find(device.instance_id) != attempted_nodes.end()) &&
                owned_devices.find(device.instance_id) == owned_devices.end()) {
              owned_devices[device.instance_id] = device;
              if (!save_owned_devices(secure_store, owned_devices, &storage_error)) {
                log_line(data_directory, L"owned_devices_save_error " + storage_error);
              }
            }
            attempted_nodes.erase(device.instance_id);
            if (newly_present || unverified_nodes.erase(device.instance_id) != 0) {
              log_line(data_directory, L"blocked_verified instance_id=" + device.instance_id);
            }
            // The approval workflow was dead until now: request_access() existed but
            // nothing called it, so the server never had a pending request to approve.
            // Ask once per device instance, only when the block is real.
            if (has_identity && api.valid() && requested_nodes.insert(device.instance_id).second) {
              usb_dlp::AccessRequestResult access_request;
              std::wstring request_error;
              if (api.request_access(
                      identity, device, make_client_request_id(), std::string("S-1-0-0"),
                      std::string("UNKNOWN"), 1,
                      std::string("Device blocked by default policy; end user requested "
                                  "temporary access from the workstation."),
                      &access_request, &request_error)) {
                log_line(data_directory, L"access_request_submitted instance_id=" +
                                             device.instance_id + L" request_id=" +
                                             usb_dlp::utf8_to_wide(access_request.request_id));
              } else {
                requested_nodes.erase(device.instance_id);
                log_line(data_directory, L"access_request_failed instance_id=" +
                                             device.instance_id + L" error=" + request_error);
              }
            }
          } else if (!dry_run) {
            const bool first_unverified = unverified_nodes.insert(device.instance_id).second;
            if (newly_present || first_unverified) {
              log_line(data_directory, L"enforce_unverified instance_id=" + device.instance_id +
                                           L" stage=" + enforcement_diagnostic +
                                           L" result=" + std::to_wstring(result));
            }
          }
          notifier.publish_denied(device, evaluation.reason_code, !dry_run && disabled, newly_present);
        } else if (owned_devices.find(device.instance_id) != owned_devices.end() ||
                   filter_usb_disk(device.instance_id)) {
          unsigned long result = 0;
          bool disabled = false;
          const bool status_known = enforcer.is_disabled(device.instance_id, &disabled, &result);
          if (!dry_run && status_known && !disabled) {
            notifier.publish_allowed(device.instance_id);
          } else if (!dry_run && enforcer.enable(device.instance_id, &result)) {
            if (owned_devices.erase(device.instance_id) != 0 &&
                !save_owned_devices(secure_store, owned_devices, &storage_error)) {
              log_line(data_directory, L"owned_devices_save_error " + storage_error);
            }
            log_line(data_directory, L"allowed instance_id=" + device.instance_id);
            notifier.publish_allowed(device.instance_id);
          } else if (!dry_run) {
            log_line(data_directory, L"enable_error instance_id=" + device.instance_id +
                                         L" result=" + std::to_wstring(result));
          }
        } else {
          notifier.publish_allowed(device.instance_id);
        }
      }
      // Devices this agent disabled in an earlier run are missing from the
      // present-only scan, so a service restart used to leave them blocked with
      // no prompt at all: the drive simply looked broken. Re-announce what is
      // still attached, and bring back anything that has since been approved.
      std::vector<std::wstring> released_owned;
      static bool owned_scan_logged = false;
      const bool log_owned_scan = !owned_scan_logged;
      if (log_owned_scan) {
        owned_scan_logged = true;
        log_line(data_directory, L"owned_device_scan count=" +
                                     std::to_wstring(owned_devices.size()) +
                                     L" enumerated=" + std::to_wstring(devices.size()));
      }
      for (const std::pair<const std::wstring, usb_dlp::WindowsDevice>& owned : owned_devices) {
        const usb_dlp::WindowsDevice& owned_device = owned.second;
        const bool in_present = present_nodes.find(owned_device.instance_id) != present_nodes.end();
        const bool still_attached = device_is_present(owned_device.instance_id);
        if (log_owned_scan) {
          log_line(data_directory, L"owned_device_check instance_id=" + owned_device.instance_id +
                                       L" enumerated=" + (in_present ? L"true" : L"false") +
                                       L" attached=" + (still_attached ? L"true" : L"false"));
        }
        if (in_present) continue;
        if (!still_attached) continue;
        usb_dlp::PolicyError owned_policy_error("");
        usb_dlp::PolicyEvaluation owned_evaluation =
            usb_dlp::evaluate_policy(owned_device.policy_device, policy, unix_ms, &owned_policy_error);
        if (owned_evaluation.decision == usb_dlp::Decision::block &&
            is_wpd_mirror(owned_device.instance_id)) {
          const std::wstring owned_parent = wpd_parent_instance_id(owned_device.instance_id);
          if (!owned_parent.empty() && policy_allows_instance(policy, owned_parent, unix_ms)) {
            owned_evaluation.decision = usb_dlp::Decision::allow;
            owned_evaluation.reason_code = "wpd_parent_allowed";
          }
        }
        if (owned_evaluation.decision == usb_dlp::Decision::allow) {
          unsigned long enable_result = 0;
          std::wstring enable_diagnostic;
          if (!dry_run &&
              enforcer.enable(owned_device.instance_id, &enable_result, &enable_diagnostic)) {
            released_owned.push_back(owned_device.instance_id);
            notifier.publish_allowed(owned_device.instance_id);
            log_line(data_directory, L"owned_device_reenabled instance_id=" +
                                         owned_device.instance_id +
                                         L" result=" + std::to_wstring(enable_result));
          }
          continue;
        }
        present_nodes.insert(owned_device.instance_id);
        notifier.publish_denied(owned_device, owned_evaluation.reason_code, true, false);
      }
      for (const std::wstring& released : released_owned) owned_devices.erase(released);
      if (!released_owned.empty()) {
        if (!save_owned_devices(secure_store, owned_devices, &storage_error)) {
          log_line(data_directory, L"owned_devices_save_error " + storage_error);
        }
      }
      for (const std::wstring& prior : previous_present_nodes) {
        if (present_nodes.find(prior) == present_nodes.end()) notifier.publish_removed(prior);
      }
      for (std::set<std::wstring>::iterator it = unverified_nodes.begin(); it != unverified_nodes.end();) {
        if (present_nodes.find(*it) == present_nodes.end()) it = unverified_nodes.erase(it);
        else ++it;
      }
      previous_present_nodes.swap(present_nodes);
    }

    if (WaitForSingleObject(stop_event, poll_ms) == WAIT_OBJECT_0) break;
  }
  notifier.stop();
  log_line(data_directory, L"agent_stop");
  return 0;
}

void WINAPI service_main(DWORD, wchar_t**) {
  g_status_handle = RegisterServiceCtrlHandlerW(kServiceName, control_handler);
  if (g_status_handle == nullptr) return;
  report_status(SERVICE_START_PENDING, NO_ERROR, 5000);
  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_stop_event == nullptr) {
    report_status(SERVICE_STOPPED, GetLastError(), 0);
    return;
  }
  report_status(SERVICE_RUNNING, NO_ERROR, 0);
  const int result = run_agent(g_stop_event);
  CloseHandle(g_stop_event);
  g_stop_event = nullptr;
  report_status(SERVICE_STOPPED, static_cast<DWORD>(result), 0);
}


}  // namespace

#ifndef USB_DLP_LEGACY_XP
namespace {

// Uninstall path: put the Device Installation Restrictions values back the way
// they were before this agent published a baseline. Values someone else changed
// in the meantime are left alone.
int restore_backstop() {
  const std::wstring common_data = usb_dlp::common_application_data_path();
  if (common_data.empty()) return ERROR_PATH_NOT_FOUND;
  const std::wstring data_directory = join_path(common_data, L"UsbDlp");
  usb_dlp::RegistryBackstop backstop;
  std::wstring error;
  if (!backstop.initialize(data_directory, &error)) {
    std::fwprintf(stderr, L"restore_backstop_failed error=\"%ls\"\n", error.c_str());
    return ERROR_ACCESS_DENIED;
  }
  if (!backstop.restore(&error)) {
    std::fwprintf(stderr, L"restore_backstop_failed error=\"%ls\"\n", error.c_str());
    return ERROR_WRITE_FAULT;
  }
  std::fwprintf(stdout, L"restore_backstop_succeeded note=\"%ls\"\n", error.c_str());
  return 0;
}

}  // namespace
#endif

int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring(argv[1]) == L"--restore-owned") {
    return restore_owned_devices();
  }
#ifndef USB_DLP_LEGACY_XP
  if (argc == 2 && std::wstring(argv[1]) == L"--restore-backstop") {
    return restore_backstop();
  }
#endif
  if (argc == 5 && std::wstring(argv[1]) == L"--configure") {
    return configure_agent(argv[2], argv[3], argv[4]);
  }
  if (argc == 3 && std::wstring(argv[1]) == L"--protect-data") {
    std::wstring error;
    if (usb_dlp::ensure_protected_directory(argv[2], &error)) {
      std::fwprintf(stdout, L"protect_data_succeeded path=\"%ls\"\n", argv[2]);
      return 0;
    }
    std::fwprintf(stderr, L"protect_data_failed path=\"%ls\" error=\"%ls\"\n",
                  argv[2], error.c_str());
    return ERROR_ACCESS_DENIED;
  }
  if (argc == 2 && std::wstring(argv[1]) == L"--console") {
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) return static_cast<int>(GetLastError());
    const int result = run_agent(stop_event);
    CloseHandle(stop_event);
    return result;
  }

  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<wchar_t*>(kServiceName), service_main},
      {nullptr, nullptr},
  };
  if (!StartServiceCtrlDispatcherW(table)) return static_cast<int>(GetLastError());
  return 0;
}
#include "usb_dlp/agent_api.h"
