#include "usb_dlp/agent_api.h"

#include "usb_dlp/json.h"
#include "usb_dlp/policy_json.h"
#include "usb_dlp/windows_device.h"

#include <windows.h>
#include <lm.h>
#include <objbase.h>
#include <sddl.h>

#include <cctype>
#include <cstdio>
#include <sstream>
#include <vector>

namespace usb_dlp {
namespace {

std::string hostname() {
  wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
  return GetComputerNameW(name, &size) ? wide_to_utf8(name) : "UNKNOWN";
}

std::string machine_sid() {
  LPUSER_MODALS_INFO_2 info = nullptr;
  if (NetUserModalsGet(nullptr, 2, reinterpret_cast<LPBYTE*>(&info)) != NERR_Success || info == nullptr) {
    return "S-1-5-21-0";
  }
  LPWSTR text = nullptr;
  std::string result = ConvertSidToStringSidW(info->usrmod2_domain_id, &text) ? wide_to_utf8(text) : "S-1-5-21-0";
  if (text != nullptr) LocalFree(text);
  NetApiBufferFree(info);
  return result;
}

std::string new_guid() {
  GUID value = {};
  if (CoCreateGuid(&value) != S_OK) return std::string();
  wchar_t text[40] = {};
  StringFromGUID2(value, text, 40);
  std::string result = wide_to_utf8(text);
  if (result.size() >= 2 && result[0] == '{') result = result.substr(1, result.size() - 2);
  for (char& character : result) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return result;
}

std::string utc_now() {
  SYSTEMTIME now = {};
  GetSystemTime(&now);
  char value[32] = {};
  sprintf_s(value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", now.wYear,
            now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
  return value;
}

std::string os_version() {
  struct RtlVersionInfo {
    ULONG size;
    ULONG major;
    ULONG minor;
    ULONG build;
    ULONG platform;
    WCHAR service_pack[128];
  };
  typedef LONG (WINAPI* RtlGetVersionFunction)(RtlVersionInfo*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) return "unknown";
  RtlGetVersionFunction get_version = reinterpret_cast<RtlGetVersionFunction>(
      GetProcAddress(ntdll, "RtlGetVersion"));
  RtlVersionInfo version = {};
  version.size = sizeof(version);
  if (get_version == nullptr || get_version(&version) != 0) return "unknown";
  return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
      std::to_string(version.build);
}

JsonValue device_json(const WindowsDevice& device) {
  JsonValue value = JsonValue::object();
  value.object_items()["instanceId"] = JsonValue(device.policy_device.instance_id);
  value.object_items()["hardwareIds"] = JsonValue::array();
  value.object_items()["deviceInstallIds"] = JsonValue::array();
  value.object_items()["vid"] = JsonValue(device.policy_device.vid);
  value.object_items()["pid"] = JsonValue(device.policy_device.pid);
  if (device.policy_device.has_serial) {
    value.object_items()["serial"] = JsonValue(device.policy_device.serial);
  } else {
    value.object_items()["serial"] = JsonValue();
  }
  value.object_items()["classGuid"] = JsonValue(device.policy_device.class_guid);
  value.object_items()["service"] = JsonValue(std::string("USBSTOR"));
  value.object_items()["friendlyName"] = JsonValue(wide_to_utf8(device.friendly_name));
  value.object_items()["transport"] = JsonValue(std::string("usb"));
  value.object_items()["capabilities"] = JsonValue::array();
  value.object_items()["manufacturer"] = JsonValue();
  value.object_items()["model"] = JsonValue();
  value.object_items()["description"] = JsonValue();
  return value;
}

bool success(unsigned long status, std::wstring* error) {
  if (status >= 200 && status < 300) return true;
  if (error != nullptr) *error = L"Server returned HTTP " + std::to_wstring(status) + L".";
  return false;
}

}  // namespace

bool encode_identity(const AgentIdentity& identity, std::string* json) {
  if (json == nullptr || identity.workstation_id.empty() || identity.api_credential.empty()) return false;
  JsonValue value = JsonValue::object();
  value.object_items()["workstationId"] = JsonValue(identity.workstation_id);
  value.object_items()["apiCredential"] = JsonValue(identity.api_credential);
  *json = write_json(value);
  return true;
}

bool decode_identity(const std::string& json, AgentIdentity* identity) {
  JsonValue value;
  std::string error;
  if (identity == nullptr || !parse_json(json, &value, &error) || value.type() != JsonValue::Type::object) return false;
  const JsonValue* workstation = value.find("workstationId");
  const JsonValue* credential = value.find("apiCredential");
  if (workstation == nullptr || credential == nullptr ||
      workstation->type() != JsonValue::Type::string || credential->type() != JsonValue::Type::string) return false;
  identity->workstation_id = workstation->string_value();
  identity->api_credential = credential->string_value();
  return true;
}

bool LegacyAgentApi::enroll(
    const std::string& enrollment_token,
    const std::string& implementation_profile,
    AgentIdentity* identity,
    std::wstring* error) const {
  JsonValue request = JsonValue::object();
  request.object_items()["schemaVersion"] = JsonValue(static_cast<std::int64_t>(2));
  request.object_items()["enrollmentToken"] = JsonValue(enrollment_token);
  request.object_items()["hostname"] = JsonValue(hostname());
  request.object_items()["machineSid"] = JsonValue(machine_sid());
  request.object_items()["agentVersion"] = JsonValue(std::string("1.1.0"));
  request.object_items()["osVersion"] = JsonValue(os_version());
  request.object_items()["architecture"] = JsonValue(
      std::string(sizeof(void*) == 8 ? "x64" : "x86"));
  request.object_items()["implementationProfile"] = JsonValue(implementation_profile);
  request.object_items()["clientNonce"] = JsonValue(new_guid());
  HttpResponse response;
  if (!client_.request(L"POST", L"api/v1/agent/enroll", write_json(request), std::string(), &response, error) ||
      !success(response.status_code, error)) return false;
  JsonValue parsed;
  std::string parse_error;
  if (!parse_json(response.body, &parsed, &parse_error) || parsed.type() != JsonValue::Type::object) return false;
  const JsonValue* workstation = parsed.find("workstationId");
  const JsonValue* credential = parsed.find("apiCredential");
  if (workstation == nullptr || credential == nullptr || workstation->type() != JsonValue::Type::string ||
      credential->type() != JsonValue::Type::string) return false;
  identity->workstation_id = workstation->string_value();
  identity->api_credential = credential->string_value();
  return true;
}

bool LegacyAgentApi::fetch_policy(
    const AgentIdentity& identity,
    CompiledPolicy* policy,
    std::string* raw_json,
    std::wstring* error) const {
  HttpResponse response;
  if (!client_.request(L"GET", L"api/v1/agent/policy", std::string(), identity.api_credential,
                       &response, error) || !success(response.status_code, error)) return false;
  std::string decode_error;
  if (!decode_compiled_policy(response.body, policy, &decode_error)) {
    if (error != nullptr) *error = L"Server returned an invalid compiled policy.";
    return false;
  }
  if (raw_json != nullptr) *raw_json = response.body;
  return true;
}

bool LegacyAgentApi::heartbeat(
    const AgentIdentity& identity,
    std::int64_t policy_version,
    int queue_depth,
    std::wstring* error) const {
  JsonValue request = JsonValue::object();
  request.object_items()["schemaVersion"] = JsonValue(static_cast<std::int64_t>(1));
  request.object_items()["agentVersion"] = JsonValue(std::string("1.1.0"));
  request.object_items()["appliedPolicyVersion"] = JsonValue(policy_version);
  request.object_items()["health"] = JsonValue(std::string(policy_version == 0 ? "fail_closed" : "healthy"));
  request.object_items()["queueDepth"] = JsonValue(static_cast<std::int64_t>(queue_depth));
  request.object_items()["lastEnforcementErrorCode"] = JsonValue();
  request.object_items()["sentAtUtc"] = JsonValue(utc_now());
  HttpResponse response;
  return client_.request(L"POST", L"api/v1/agent/heartbeat", write_json(request), identity.api_credential,
                         &response, error) && success(response.status_code, error);
}

bool LegacyAgentApi::request_access(
    const AgentIdentity& identity,
    const WindowsDevice& device,
    const std::string& client_request_id,
    const std::string& user_sid,
    const std::string& username,
    int requested_duration_days,
    const std::string& justification,
    AccessRequestResult* result,
    std::wstring* error) const {
  if (result == nullptr || (requested_duration_days != 1 && requested_duration_days != 7 &&
                            requested_duration_days != 30 && requested_duration_days != 90) ||
      justification.size() < 10 || justification.size() > 2000) {
    if (error != nullptr) *error = L"Access request validation failed.";
    return false;
  }
  JsonValue request = JsonValue::object();
  request.object_items()["schemaVersion"] = JsonValue(static_cast<std::int64_t>(1));
  request.object_items()["clientRequestId"] = JsonValue(client_request_id);
  request.object_items()["occurredAtUtc"] = JsonValue(utc_now());
  request.object_items()["userSid"] = JsonValue(user_sid.empty() ? std::string("S-1-0-0") : user_sid);
  request.object_items()["username"] = JsonValue(username.empty() ? std::string("UNKNOWN") : username);
  request.object_items()["device"] = device_json(device);
  request.object_items()["requestedDurationDays"] =
      JsonValue(static_cast<std::int64_t>(requested_duration_days));
  request.object_items()["justification"] = JsonValue(justification);
  HttpResponse response;
  if (!client_.request(L"POST", L"api/v1/agent/requests", write_json(request),
                       identity.api_credential, &response, error) ||
      !success(response.status_code, error)) return false;
  JsonValue parsed;
  std::string parse_error;
  if (!parse_json(response.body, &parsed, &parse_error) ||
      parsed.type() != JsonValue::Type::object) {
    if (error != nullptr) *error = L"Server returned an invalid access-request response.";
    return false;
  }
  const JsonValue* request_id = parsed.find("requestId");
  const JsonValue* status = parsed.find("status");
  if (request_id == nullptr || status == nullptr ||
      request_id->type() != JsonValue::Type::string || status->type() != JsonValue::Type::string) {
    if (error != nullptr) *error = L"Access-request response is missing required fields.";
    return false;
  }
  result->request_id = request_id->string_value();
  result->status = status->string_value();
  const JsonValue* retry = parsed.find("retryAfterUtc");
  result->retry_after_utc = retry != nullptr && retry->type() == JsonValue::Type::string
                                ? retry->string_value()
                                : std::string();
  return true;
}

}  // namespace usb_dlp
