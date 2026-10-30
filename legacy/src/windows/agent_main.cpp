#include "usb_dlp/agent_api.h"
#include "usb_dlp/json.h"
#include "usb_dlp/notifier_bridge.h"
#include "usb_dlp/policy.h"
#include "usb_dlp/policy_json.h"
#include "usb_dlp/secure_store.h"
#include "usb_dlp/windows_device.h"

#include <windows.h>

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

bool read_bool(const std::wstring& ini, const wchar_t* key, bool default_value) {
  return GetPrivateProfileIntW(L"Agent", key, default_value ? 1 : 0, ini.c_str()) != 0;
}

std::wstring read_ini_string(
    const std::wstring& ini,
    const wchar_t* key,
    const wchar_t* default_value) {
  std::vector<wchar_t> value(2048, L'\0');
  GetPrivateProfileStringW(L"Agent", key, default_value, &value[0],
                           static_cast<DWORD>(value.size()), ini.c_str());
  return &value[0];
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
  const std::wstring ini_contents =
      L"[Agent]\r\n"
      L"DryRun=0\r\n"
      L"DebugLog=" + debug_log + L"\r\n"
      L"ServerUrl=" + server_url + L"\r\n"
      L"AllowInsecureHttp=1\r\n"
      L"EnrollmentTokenFile=" + token_destination + L"\r\n"
      L"LocalPollSeconds=2\r\n"
      L"PolicyPollSeconds=30\r\n";
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
  UINT seconds = GetPrivateProfileIntW(L"Agent", L"LocalPollSeconds", 2, ini.c_str());
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
  UINT policy_poll_seconds = GetPrivateProfileIntW(
      L"Agent", L"PolicyPollSeconds", 30, ini.c_str());
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
  OwnedDevices owned_devices;
  if (!load_owned_devices(secure_store, &owned_devices, &storage_error)) {
    log_line(data_directory, L"owned_devices_load_error " + storage_error);
  }
  std::set<std::wstring> unverified_nodes;
  std::set<std::wstring> attempted_nodes;
  std::set<std::wstring> previous_present_nodes;
  std::set<std::wstring> previous_device_diagnostics;
  DWORD last_policy_poll = GetTickCount() - policy_poll_ms;
  log_line(data_directory, dry_run ? L"agent_start dry_run=true" : L"agent_start dry_run=false default_deny=true");
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
      FILETIME file_time = {};
      GetSystemTimeAsFileTime(&file_time);
      ULARGE_INTEGER ticks = {};
      ticks.LowPart = file_time.dwLowDateTime;
      ticks.HighPart = file_time.dwHighDateTime;
      const std::int64_t unix_ms = static_cast<std::int64_t>((ticks.QuadPart - 116444736000000000ULL) / 10000ULL);
      for (const usb_dlp::WindowsDevice& device : devices) {
        present_nodes.insert(device.instance_id);
        const bool newly_present =
            previous_present_nodes.find(device.instance_id) == previous_present_nodes.end();
        usb_dlp::PolicyError policy_error("");
        const usb_dlp::PolicyEvaluation evaluation =
            usb_dlp::evaluate_policy(device.policy_device, policy, unix_ms, &policy_error);
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

int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring(argv[1]) == L"--restore-owned") {
    return restore_owned_devices();
  }
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
