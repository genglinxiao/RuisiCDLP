#include <winsock2.h>
#include <ws2tcpip.h>

#include "usb_dlp/json.h"
#include "usb_dlp/secure_store.h"
#include "usb_dlp/windows_device.h"

#include <windows.h>
#include <wincrypt.h>
#include <objbase.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

const wchar_t* const kServiceName = L"UsbDlp.LegacyServer";
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status = {};
HANDLE g_stop_event = nullptr;

struct Workstation {
  std::string id;
  std::string hostname;
  std::string machine_sid;
  std::string credential_hash;
  std::string profile;
  std::string last_seen_utc;
};

struct RequestRecord {
  std::string id;
  std::string workstation_id;
  std::string status;
  std::string instance_id;
  std::string friendly_name;
  std::string vid;
  std::string pid;
  std::string user_sid;
  std::string username;
  std::string justification;
  std::string created_at_utc;
  int requested_days = 30;
};

struct Grant {
  std::string entry_id;
  std::string request_id;
  std::string workstation_id;
  std::string instance_id;
  std::string expires_at_utc;
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;
  std::string body;
};

struct HttpResult {
  int status = 500;
  std::string body;
};

std::wstring join_path(const std::wstring& parent, const wchar_t* child) {
  if (parent.empty()) return std::wstring();
  return parent + (parent[parent.size() - 1] == L'\\' ? L"" : L"\\") + child;
}

std::string lower_ascii(const std::string& value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

std::string trim(const std::string& value) {
  std::size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
  std::size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
  return value.substr(start, end - start);
}

std::string utc_from_filetime(const FILETIME& time) {
  SYSTEMTIME system = {};
  FileTimeToSystemTime(&time, &system);
  char text[32] = {};
  sprintf_s(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", system.wYear,
            system.wMonth, system.wDay, system.wHour, system.wMinute, system.wSecond,
            system.wMilliseconds);
  return text;
}

std::string utc_now() {
  FILETIME time = {};
  GetSystemTimeAsFileTime(&time);
  return utc_from_filetime(time);
}

std::string utc_after_days(int days) {
  FILETIME time = {};
  GetSystemTimeAsFileTime(&time);
  ULARGE_INTEGER ticks = {};
  ticks.LowPart = time.dwLowDateTime;
  ticks.HighPart = time.dwHighDateTime;
  ticks.QuadPart += static_cast<ULONGLONG>(days) * 24ULL * 60ULL * 60ULL * 10000000ULL;
  time.dwLowDateTime = ticks.LowPart;
  time.dwHighDateTime = ticks.HighPart;
  return utc_from_filetime(time);
}

std::string guid_text() {
  GUID value = {};
  if (CoCreateGuid(&value) != S_OK) return std::string();
  wchar_t text[40] = {};
  StringFromGUID2(value, text, 40);
  std::string result = usb_dlp::wide_to_utf8(text);
  if (result.size() >= 2) result = result.substr(1, result.size() - 2);
  return lower_ascii(result);
}

std::string hex(const BYTE* bytes, DWORD size) {
  const char* digits = "0123456789abcdef";
  std::string result(size * 2, '0');
  for (DWORD index = 0; index < size; ++index) {
    result[index * 2] = digits[(bytes[index] >> 4) & 0x0F];
    result[index * 2 + 1] = digits[bytes[index] & 0x0F];
  }
  return result;
}

std::string sha256(const std::string& value) {
  HCRYPTPROV provider = 0;
  HCRYPTHASH hash = 0;
  BYTE digest[32] = {};
  DWORD digest_size = sizeof(digest);
  std::string result;
  if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
      CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
      CryptHashData(hash, reinterpret_cast<const BYTE*>(value.data()),
                    static_cast<DWORD>(value.size()), 0) &&
      CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_size, 0)) {
    result = hex(digest, digest_size);
  }
  if (hash != 0) CryptDestroyHash(hash);
  if (provider != 0) CryptReleaseContext(provider, 0);
  SecureZeroMemory(digest, sizeof(digest));
  return result;
}

std::string random_secret() {
  HCRYPTPROV provider = 0;
  BYTE bytes[32] = {};
  std::string result;
  if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL,
                           CRYPT_VERIFYCONTEXT | CRYPT_SILENT) &&
      CryptGenRandom(provider, sizeof(bytes), bytes)) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned int accumulator = 0;
    int bits = 0;
    for (std::size_t index = 0; index < sizeof(bytes); ++index) {
      accumulator = (accumulator << 8) | bytes[index];
      bits += 8;
      while (bits >= 6) {
        bits -= 6;
        result.push_back(alphabet[(accumulator >> bits) & 0x3F]);
      }
    }
    if (bits > 0) result.push_back(alphabet[(accumulator << (6 - bits)) & 0x3F]);
  }
  if (provider != 0) CryptReleaseContext(provider, 0);
  SecureZeroMemory(bytes, sizeof(bytes));
  return result;
}

std::string decode_base64(const std::string& value) {
  DWORD size = 0;
  if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()),
                            CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr) || size == 0) {
    return std::string();
  }
  std::vector<BYTE> bytes(size);
  if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()),
                            CRYPT_STRING_BASE64, &bytes[0], &size, nullptr, nullptr)) {
    return std::string();
  }
  return std::string(reinterpret_cast<const char*>(&bytes[0]), size);
}

bool constant_time_equal(const std::string& left, const std::string& right) {
  unsigned int difference = static_cast<unsigned int>(left.size() ^ right.size());
  const std::size_t count = left.size() > right.size() ? left.size() : right.size();
  for (std::size_t index = 0; index < count; ++index) {
    const unsigned char a = index < left.size() ? static_cast<unsigned char>(left[index]) : 0;
    const unsigned char b = index < right.size() ? static_cast<unsigned char>(right[index]) : 0;
    difference |= a ^ b;
  }
  return difference == 0;
}

const usb_dlp::JsonValue* field(
    const usb_dlp::JsonValue& object,
    const char* name,
    usb_dlp::JsonValue::Type type) {
  const usb_dlp::JsonValue* value = object.find(name);
  return value != nullptr && value->type() == type ? value : nullptr;
}

usb_dlp::JsonValue workstation_json(const Workstation& item) {
  usb_dlp::JsonValue value = usb_dlp::JsonValue::object();
  value.object_items()["id"] = usb_dlp::JsonValue(item.id);
  value.object_items()["hostname"] = usb_dlp::JsonValue(item.hostname);
  value.object_items()["machineSid"] = usb_dlp::JsonValue(item.machine_sid);
  value.object_items()["credentialHash"] = usb_dlp::JsonValue(item.credential_hash);
  value.object_items()["profile"] = usb_dlp::JsonValue(item.profile);
  value.object_items()["lastSeenUtc"] = usb_dlp::JsonValue(item.last_seen_utc);
  return value;
}

usb_dlp::JsonValue request_json(const RequestRecord& item) {
  usb_dlp::JsonValue value = usb_dlp::JsonValue::object();
  value.object_items()["id"] = usb_dlp::JsonValue(item.id);
  value.object_items()["workstationId"] = usb_dlp::JsonValue(item.workstation_id);
  value.object_items()["status"] = usb_dlp::JsonValue(item.status);
  value.object_items()["instanceId"] = usb_dlp::JsonValue(item.instance_id);
  value.object_items()["friendlyName"] = usb_dlp::JsonValue(item.friendly_name);
  value.object_items()["vid"] = usb_dlp::JsonValue(item.vid);
  value.object_items()["pid"] = usb_dlp::JsonValue(item.pid);
  value.object_items()["userSid"] = usb_dlp::JsonValue(item.user_sid);
  value.object_items()["username"] = usb_dlp::JsonValue(item.username);
  value.object_items()["justification"] = usb_dlp::JsonValue(item.justification);
  value.object_items()["createdAtUtc"] = usb_dlp::JsonValue(item.created_at_utc);
  value.object_items()["requestedDays"] =
      usb_dlp::JsonValue(static_cast<std::int64_t>(item.requested_days));
  return value;
}

usb_dlp::JsonValue grant_json(const Grant& item) {
  usb_dlp::JsonValue value = usb_dlp::JsonValue::object();
  value.object_items()["entryId"] = usb_dlp::JsonValue(item.entry_id);
  value.object_items()["requestId"] = usb_dlp::JsonValue(item.request_id);
  value.object_items()["workstationId"] = usb_dlp::JsonValue(item.workstation_id);
  value.object_items()["instanceId"] = usb_dlp::JsonValue(item.instance_id);
  value.object_items()["expiresAtUtc"] = usb_dlp::JsonValue(item.expires_at_utc);
  return value;
}

class StateStore {
 public:
  StateStore(
      const std::wstring& data_directory,
      const std::string& enrollment_token,
      const std::string& admin_password)
      : secure_store_(data_directory), enrollment_token_hash_(sha256(enrollment_token)),
        admin_password_hash_(sha256(admin_password)), policy_version_(1) {
    InitializeCriticalSection(&lock_);
  }

  ~StateStore() { DeleteCriticalSection(&lock_); }

  bool load(std::wstring* error) {
    if (!secure_store_.exists(L"server-state.dat")) return save(error);
    std::string encoded;
    if (!secure_store_.load(L"server-state.dat", &encoded, error)) return false;
    usb_dlp::JsonValue root;
    std::string parse_error;
    if (!usb_dlp::parse_json(encoded, &root, &parse_error) ||
        root.type() != usb_dlp::JsonValue::Type::object) {
      if (error != nullptr) *error = L"The encrypted server state is invalid.";
      return false;
    }
    const usb_dlp::JsonValue* version = field(root, "policyVersion", usb_dlp::JsonValue::Type::number);
    const usb_dlp::JsonValue* workstations = field(root, "workstations", usb_dlp::JsonValue::Type::array);
    const usb_dlp::JsonValue* requests = field(root, "requests", usb_dlp::JsonValue::Type::array);
    const usb_dlp::JsonValue* grants = field(root, "grants", usb_dlp::JsonValue::Type::array);
    if (version == nullptr || workstations == nullptr || requests == nullptr || grants == nullptr) return false;
    policy_version_ = version->number_value();
    for (const usb_dlp::JsonValue& value : workstations->array_items()) decode_workstation(value);
    for (const usb_dlp::JsonValue& value : requests->array_items()) decode_request(value);
    for (const usb_dlp::JsonValue& value : grants->array_items()) decode_grant(value);
    return true;
  }

  HttpResult enroll(const std::string& body) {
    usb_dlp::JsonValue request;
    std::string error;
    if (!usb_dlp::parse_json(body, &request, &error) || request.type() != usb_dlp::JsonValue::Type::object) {
      return json_error(400, "invalid_json");
    }
    const usb_dlp::JsonValue* token = field(request, "enrollmentToken", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* hostname = field(request, "hostname", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* sid = field(request, "machineSid", usb_dlp::JsonValue::Type::string);
    if (token == nullptr || hostname == nullptr || sid == nullptr || hostname->string_value().empty() ||
        !constant_time_equal(sha256(token->string_value()), enrollment_token_hash_)) {
      return json_error(403, "invalid_enrollment");
    }
    EnterCriticalSection(&lock_);
    Workstation* workstation = nullptr;
    for (Workstation& item : workstations_) {
      if (item.machine_sid == sid->string_value()) workstation = &item;
    }
    const bool created = workstation == nullptr;
    if (created) {
      Workstation item;
      item.id = guid_text();
      item.machine_sid = sid->string_value();
      workstations_.push_back(item);
      workstation = &workstations_.back();
    }
    const std::string credential = workstation->id + "." + random_secret();
    workstation->hostname = hostname->string_value();
    workstation->credential_hash = sha256(credential);
    workstation->profile = "unknown";
    const usb_dlp::JsonValue* profile = request.find("implementationProfile");
    if (profile != nullptr && profile->type() == usb_dlp::JsonValue::Type::string) {
      workstation->profile = profile->string_value();
    }
    workstation->last_seen_utc = utc_now();
    std::wstring save_error;
    const bool saved = save_locked(&save_error);
    const std::string workstation_id = workstation->id;
    LeaveCriticalSection(&lock_);
    if (!saved) return json_error(500, "state_write_failed");
    usb_dlp::JsonValue response = usb_dlp::JsonValue::object();
    const usb_dlp::JsonValue* schema = request.find("schemaVersion");
    response.object_items()["schemaVersion"] = usb_dlp::JsonValue(
        schema != nullptr && schema->type() == usb_dlp::JsonValue::Type::number
            ? schema->number_value()
            : static_cast<std::int64_t>(1));
    response.object_items()["workstationId"] = usb_dlp::JsonValue(workstation_id);
    response.object_items()["apiCredential"] = usb_dlp::JsonValue(credential);
    response.object_items()["serverTimeUtc"] = usb_dlp::JsonValue(utc_now());
    response.object_items()["policyPollSeconds"] = usb_dlp::JsonValue(static_cast<std::int64_t>(30));
    return HttpResult{created ? 201 : 200, usb_dlp::write_json(response)};
  }

  HttpResult policy(const std::string& bearer) {
    EnterCriticalSection(&lock_);
    Workstation* workstation = authenticate_locked(bearer);
    if (workstation == nullptr) {
      LeaveCriticalSection(&lock_);
      return json_error(401, "invalid_credential");
    }
    workstation->last_seen_utc = utc_now();
    usb_dlp::JsonValue response = usb_dlp::JsonValue::object();
    response.object_items()["schemaVersion"] = usb_dlp::JsonValue(static_cast<std::int64_t>(1));
    response.object_items()["policyVersion"] = usb_dlp::JsonValue(policy_version_);
    response.object_items()["groupMode"] = usb_dlp::JsonValue(std::string("default_deny"));
    response.object_items()["entries"] = usb_dlp::JsonValue::array();
    for (const Grant& grant : grants_) {
      if (grant.workstation_id != workstation->id || grant.expires_at_utc <= utc_now()) continue;
      usb_dlp::JsonValue entry = usb_dlp::JsonValue::object();
      entry.object_items()["id"] = usb_dlp::JsonValue(grant.entry_id);
      entry.object_items()["matchType"] = usb_dlp::JsonValue(std::string("instance_id"));
      entry.object_items()["vid"] = usb_dlp::JsonValue();
      entry.object_items()["pid"] = usb_dlp::JsonValue();
      entry.object_items()["serial"] = usb_dlp::JsonValue();
      entry.object_items()["instanceId"] = usb_dlp::JsonValue(grant.instance_id);
      entry.object_items()["deviceClass"] = usb_dlp::JsonValue();
      entry.object_items()["action"] = usb_dlp::JsonValue(std::string("allow"));
      entry.object_items()["mandatory"] = usb_dlp::JsonValue(false);
      entry.object_items()["scopeType"] = usb_dlp::JsonValue(std::string("workstation"));
      entry.object_items()["scopeId"] = usb_dlp::JsonValue(grant.workstation_id);
      entry.object_items()["expiresAtUtc"] = usb_dlp::JsonValue(grant.expires_at_utc);
      response.object_items()["entries"].array_items().push_back(entry);
    }
    const std::string body = usb_dlp::write_json(response);
    LeaveCriticalSection(&lock_);
    return HttpResult{200, body};
  }

  HttpResult heartbeat(const std::string& bearer) {
    EnterCriticalSection(&lock_);
    Workstation* workstation = authenticate_locked(bearer);
    if (workstation == nullptr) {
      LeaveCriticalSection(&lock_);
      return json_error(401, "invalid_credential");
    }
    workstation->last_seen_utc = utc_now();
    const std::int64_t version = policy_version_;
    LeaveCriticalSection(&lock_);
    usb_dlp::JsonValue response = usb_dlp::JsonValue::object();
    response.object_items()["schemaVersion"] = usb_dlp::JsonValue(static_cast<std::int64_t>(1));
    response.object_items()["serverTimeUtc"] = usb_dlp::JsonValue(utc_now());
    response.object_items()["policyChanged"] = usb_dlp::JsonValue(true);
    response.object_items()["currentPolicyVersion"] = usb_dlp::JsonValue(version);
    return HttpResult{200, usb_dlp::write_json(response)};
  }

  HttpResult create_request(const std::string& bearer, const std::string& body) {
    usb_dlp::JsonValue input;
    std::string parse_error;
    if (!usb_dlp::parse_json(body, &input, &parse_error) || input.type() != usb_dlp::JsonValue::Type::object) {
      return json_error(400, "invalid_json");
    }
    const usb_dlp::JsonValue* device = field(input, "device", usb_dlp::JsonValue::Type::object);
    const usb_dlp::JsonValue* client_id = field(input, "clientRequestId", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* user_sid = field(input, "userSid", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* username_value = field(input, "username", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* days = field(input, "requestedDurationDays", usb_dlp::JsonValue::Type::number);
    const usb_dlp::JsonValue* justification = field(input, "justification", usb_dlp::JsonValue::Type::string);
    if (device == nullptr || client_id == nullptr || user_sid == nullptr || username_value == nullptr ||
        days == nullptr || justification == nullptr || justification->string_value().size() < 10 ||
        justification->string_value().size() > 2000) return json_error(422, "invalid_request");
    const usb_dlp::JsonValue* instance = field(*device, "instanceId", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* friendly = field(*device, "friendlyName", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* vid = field(*device, "vid", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* pid = field(*device, "pid", usb_dlp::JsonValue::Type::string);
    if (instance == nullptr || friendly == nullptr || vid == nullptr || pid == nullptr) {
      return json_error(422, "invalid_device");
    }
    EnterCriticalSection(&lock_);
    Workstation* workstation = authenticate_locked(bearer);
    if (workstation == nullptr) {
      LeaveCriticalSection(&lock_);
      return json_error(401, "invalid_credential");
    }
    for (const RequestRecord& existing : requests_) {
      if (existing.id == client_id->string_value() ||
          (existing.workstation_id == workstation->id && existing.user_sid == user_sid->string_value() &&
           existing.instance_id == instance->string_value() && existing.status == "pending")) {
        const HttpResult result = request_response(existing, "recent", 200);
        LeaveCriticalSection(&lock_);
        return result;
      }
    }
    RequestRecord record;
    record.id = client_id->string_value();
    record.workstation_id = workstation->id;
    record.status = "pending";
    record.instance_id = instance->string_value();
    record.friendly_name = friendly->string_value();
    record.vid = vid->string_value();
    record.pid = pid->string_value();
    record.user_sid = user_sid->string_value();
    record.username = username_value->string_value();
    record.justification = justification->string_value();
    record.created_at_utc = utc_now();
    record.requested_days = static_cast<int>(days->number_value());
    requests_.push_back(record);
    std::wstring save_error;
    const bool saved = save_locked(&save_error);
    const HttpResult result = saved ? request_response(record, "pending", 201)
                                    : json_error(500, "state_write_failed");
    LeaveCriticalSection(&lock_);
    return result;
  }

  HttpResult admin_requests(const std::string& authorization) {
    if (!admin_authorized(authorization)) return json_error(401, "invalid_admin_credential");
    EnterCriticalSection(&lock_);
    usb_dlp::JsonValue response = usb_dlp::JsonValue::object();
    response.object_items()["schemaVersion"] = usb_dlp::JsonValue(static_cast<std::int64_t>(1));
    response.object_items()["items"] = usb_dlp::JsonValue::array();
    for (const RequestRecord& item : requests_) {
      usb_dlp::JsonValue value = request_json(item);
      value.object_items().erase("credentialHash");
      response.object_items()["items"].array_items().push_back(value);
    }
    const std::string body = usb_dlp::write_json(response);
    LeaveCriticalSection(&lock_);
    return HttpResult{200, body};
  }

  HttpResult admin_workstations(const std::string& authorization) {
    if (!admin_authorized(authorization)) return json_error(401, "invalid_admin_credential");
    EnterCriticalSection(&lock_);
    usb_dlp::JsonValue response = usb_dlp::JsonValue::object();
    response.object_items()["schemaVersion"] = usb_dlp::JsonValue(static_cast<std::int64_t>(1));
    response.object_items()["items"] = usb_dlp::JsonValue::array();
    for (const Workstation& item : workstations_) {
      usb_dlp::JsonValue value = workstation_json(item);
      value.object_items().erase("credentialHash");
      response.object_items()["items"].array_items().push_back(value);
    }
    const std::string body = usb_dlp::write_json(response);
    LeaveCriticalSection(&lock_);
    return HttpResult{200, body};
  }

  HttpResult admin_decide(
      const std::string& authorization,
      const std::string& request_id,
      const std::string& decision,
      int days) {
    if (!admin_authorized(authorization)) return json_error(401, "invalid_admin_credential");
    const bool success = decision == "revoked" ? revoke(request_id) : decide(request_id, decision, days);
    if (!success) return json_error(409, "invalid_request_state");
    usb_dlp::JsonValue response = usb_dlp::JsonValue::object();
    response.object_items()["schemaVersion"] = usb_dlp::JsonValue(static_cast<std::int64_t>(1));
    response.object_items()["requestId"] = usb_dlp::JsonValue(request_id);
    response.object_items()["status"] = usb_dlp::JsonValue(decision);
    return HttpResult{200, usb_dlp::write_json(response)};
  }

  bool list_requests() {
    EnterCriticalSection(&lock_);
    std::cout << "ID\tSTATUS\tWORKSTATION\tDEVICE\tUSER\n";
    for (const RequestRecord& item : requests_) {
      std::cout << item.id << '\t' << item.status << '\t' << item.workstation_id << '\t'
                << item.friendly_name << " [" << item.vid << ':' << item.pid << "]\t"
                << item.username << '\n';
    }
    LeaveCriticalSection(&lock_);
    return true;
  }

  bool list_workstations(const std::string& query) {
    const std::string normalized = lower_ascii(query);
    EnterCriticalSection(&lock_);
    std::cout << "ID\tHOSTNAME\tPROFILE\tLAST SEEN\n";
    for (const Workstation& item : workstations_) {
      if (!normalized.empty() && lower_ascii(item.id).find(normalized) == std::string::npos &&
          lower_ascii(item.hostname).find(normalized) == std::string::npos &&
          lower_ascii(item.machine_sid).find(normalized) == std::string::npos) continue;
      std::cout << item.id << '\t' << item.hostname << '\t' << item.profile << '\t'
                << item.last_seen_utc << '\n';
    }
    LeaveCriticalSection(&lock_);
    return true;
  }

  bool decide(const std::string& request_id, const std::string& decision, int days) {
    EnterCriticalSection(&lock_);
    RequestRecord* request = nullptr;
    for (RequestRecord& item : requests_) if (item.id == request_id) request = &item;
    if (request == nullptr || request->status != "pending") {
      LeaveCriticalSection(&lock_);
      return false;
    }
    request->status = decision;
    if (decision == "approved") {
      Grant grant;
      grant.entry_id = guid_text();
      grant.request_id = request->id;
      grant.workstation_id = request->workstation_id;
      grant.instance_id = request->instance_id;
      grant.expires_at_utc = utc_after_days(days);
      grants_.push_back(grant);
      ++policy_version_;
    }
    std::wstring error;
    const bool result = save_locked(&error);
    LeaveCriticalSection(&lock_);
    return result;
  }

  bool revoke(const std::string& request_id) {
    EnterCriticalSection(&lock_);
    bool changed = false;
    for (RequestRecord& item : requests_) {
      if (item.id == request_id && item.status == "approved") {
        item.status = "revoked";
        changed = true;
      }
    }
    grants_.erase(std::remove_if(grants_.begin(), grants_.end(), [&](const Grant& item) {
      return item.request_id == request_id;
    }), grants_.end());
    if (changed) ++policy_version_;
    std::wstring error;
    const bool result = changed && save_locked(&error);
    LeaveCriticalSection(&lock_);
    return result;
  }

 private:
  usb_dlp::SecureStore secure_store_;
  std::string enrollment_token_hash_;
  std::string admin_password_hash_;
  std::int64_t policy_version_;
  std::vector<Workstation> workstations_;
  std::vector<RequestRecord> requests_;
  std::vector<Grant> grants_;
  CRITICAL_SECTION lock_;

  static HttpResult json_error(int status, const std::string& code) {
    usb_dlp::JsonValue value = usb_dlp::JsonValue::object();
    value.object_items()["error"] = usb_dlp::JsonValue(code);
    return HttpResult{status, usb_dlp::write_json(value)};
  }

  static HttpResult request_response(const RequestRecord& request, const std::string& status, int code) {
    usb_dlp::JsonValue value = usb_dlp::JsonValue::object();
    value.object_items()["schemaVersion"] = usb_dlp::JsonValue(static_cast<std::int64_t>(1));
    value.object_items()["requestId"] = usb_dlp::JsonValue(request.id);
    value.object_items()["status"] = usb_dlp::JsonValue(status);
    value.object_items()["createdAtUtc"] = usb_dlp::JsonValue(request.created_at_utc);
    value.object_items()["retryAfterUtc"] = usb_dlp::JsonValue(utc_after_days(0));
    return HttpResult{code, usb_dlp::write_json(value)};
  }

  Workstation* authenticate_locked(const std::string& bearer) {
    if (bearer.empty()) return nullptr;
    const std::string hash = sha256(bearer);
    for (Workstation& item : workstations_) {
      if (constant_time_equal(item.credential_hash, hash)) return &item;
    }
    return nullptr;
  }

  bool admin_authorized(const std::string& authorization) const {
    if (authorization.size() < 7 || lower_ascii(authorization.substr(0, 6)) != "basic ") return false;
    const std::string decoded = decode_base64(authorization.substr(6));
    const std::string::size_type separator = decoded.find(':');
    if (separator == std::string::npos || decoded.substr(0, separator) != "admin") return false;
    return constant_time_equal(sha256(decoded.substr(separator + 1)), admin_password_hash_);
  }

  bool save(std::wstring* error) {
    EnterCriticalSection(&lock_);
    const bool result = save_locked(error);
    LeaveCriticalSection(&lock_);
    return result;
  }

  bool save_locked(std::wstring* error) {
    usb_dlp::JsonValue root = usb_dlp::JsonValue::object();
    root.object_items()["schemaVersion"] = usb_dlp::JsonValue(static_cast<std::int64_t>(1));
    root.object_items()["policyVersion"] = usb_dlp::JsonValue(policy_version_);
    root.object_items()["workstations"] = usb_dlp::JsonValue::array();
    root.object_items()["requests"] = usb_dlp::JsonValue::array();
    root.object_items()["grants"] = usb_dlp::JsonValue::array();
    for (const Workstation& item : workstations_) {
      root.object_items()["workstations"].array_items().push_back(workstation_json(item));
    }
    for (const RequestRecord& item : requests_) {
      root.object_items()["requests"].array_items().push_back(request_json(item));
    }
    for (const Grant& item : grants_) {
      root.object_items()["grants"].array_items().push_back(grant_json(item));
    }
    return secure_store_.save(L"server-state.dat", usb_dlp::write_json(root), error);
  }

  void decode_workstation(const usb_dlp::JsonValue& value) {
    const usb_dlp::JsonValue* id = field(value, "id", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* hostname = field(value, "hostname", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* sid = field(value, "machineSid", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* credential = field(value, "credentialHash", usb_dlp::JsonValue::Type::string);
    if (id == nullptr || hostname == nullptr || sid == nullptr || credential == nullptr) return;
    Workstation item;
    item.id = id->string_value(); item.hostname = hostname->string_value();
    item.machine_sid = sid->string_value(); item.credential_hash = credential->string_value();
    const usb_dlp::JsonValue* profile = value.find("profile");
    const usb_dlp::JsonValue* seen = value.find("lastSeenUtc");
    item.profile = profile != nullptr && profile->type() == usb_dlp::JsonValue::Type::string ? profile->string_value() : "unknown";
    item.last_seen_utc = seen != nullptr && seen->type() == usb_dlp::JsonValue::Type::string ? seen->string_value() : std::string();
    workstations_.push_back(item);
  }

  void decode_request(const usb_dlp::JsonValue& value) {
    const usb_dlp::JsonValue* id = field(value, "id", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* workstation = field(value, "workstationId", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* status = field(value, "status", usb_dlp::JsonValue::Type::string);
    const usb_dlp::JsonValue* instance = field(value, "instanceId", usb_dlp::JsonValue::Type::string);
    if (id == nullptr || workstation == nullptr || status == nullptr || instance == nullptr) return;
    RequestRecord item;
    item.id = id->string_value(); item.workstation_id = workstation->string_value();
    item.status = status->string_value(); item.instance_id = instance->string_value();
    const char* strings[] = {"friendlyName", "vid", "pid", "userSid", "username", "justification", "createdAtUtc"};
    std::string* targets[] = {&item.friendly_name, &item.vid, &item.pid, &item.user_sid, &item.username, &item.justification, &item.created_at_utc};
    for (int index = 0; index < 7; ++index) {
      const usb_dlp::JsonValue* source = value.find(strings[index]);
      if (source != nullptr && source->type() == usb_dlp::JsonValue::Type::string) *targets[index] = source->string_value();
    }
    const usb_dlp::JsonValue* days = value.find("requestedDays");
    if (days != nullptr && days->type() == usb_dlp::JsonValue::Type::number) item.requested_days = static_cast<int>(days->number_value());
    requests_.push_back(item);
  }

  void decode_grant(const usb_dlp::JsonValue& value) {
    const char* names[] = {"entryId", "requestId", "workstationId", "instanceId", "expiresAtUtc"};
    std::string values[5];
    for (int index = 0; index < 5; ++index) {
      const usb_dlp::JsonValue* source = field(value, names[index], usb_dlp::JsonValue::Type::string);
      if (source == nullptr) return;
      values[index] = source->string_value();
    }
    Grant item;
    item.entry_id = values[0]; item.request_id = values[1]; item.workstation_id = values[2];
    item.instance_id = values[3]; item.expires_at_utc = values[4];
    grants_.push_back(item);
  }
};

bool read_small_file(const std::wstring& path, std::string* value) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD size = GetFileSize(file, nullptr);
  if (size == INVALID_FILE_SIZE || size == 0 || size > 4096) { CloseHandle(file); return false; }
  value->assign(size, '\0');
  DWORD read = 0;
  const BOOL success = ReadFile(file, &(*value)[0], size, &read, nullptr);
  CloseHandle(file);
  if (!success || read != size) return false;
  *value = trim(*value);
  return !value->empty();
}

bool contains_line_break(const std::wstring& value) {
  return value.find(L'\r') != std::wstring::npos || value.find(L'\n') != std::wstring::npos;
}

int configure_server(
    const std::wstring& token_source,
    const std::wstring& password_source,
    const std::wstring& port_text) {
  wchar_t* port_end = nullptr;
  const unsigned long port = std::wcstoul(port_text.c_str(), &port_end, 10);
  if (contains_line_break(port_text) || port_text.empty() || port_end == nullptr ||
      *port_end != L'\0' || port < 1 || port > 65535) {
    std::fwprintf(stderr, L"configure_failed error=\"invalid port\"\n");
    return ERROR_INVALID_PARAMETER;
  }
  std::string token;
  std::string password;
  const bool credentials_valid =
      read_small_file(token_source, &token) && token.size() >= 32 &&
      read_small_file(password_source, &password) && password.size() >= 14;
  if (!token.empty()) SecureZeroMemory(&token[0], token.size());
  if (!password.empty()) SecureZeroMemory(&password[0], password.size());
  if (!credentials_valid) {
    std::fwprintf(stderr, L"configure_failed error=\"credential file missing or too short\"\n");
    return ERROR_INVALID_DATA;
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
  const std::wstring token_destination = join_path(data_directory, L"enrollment-token.txt");
  const std::wstring password_destination = join_path(data_directory, L"admin-password.txt");
  if (!CopyFileW(token_source.c_str(), token_destination.c_str(), FALSE)) {
    std::fwprintf(stderr, L"configure_failed data_directory=\"%ls\" operation=\"CopyFile(token)\" error=%lu\n",
                  data_directory.c_str(), GetLastError());
    return ERROR_WRITE_FAULT;
  }
  if (!CopyFileW(password_source.c_str(), password_destination.c_str(), FALSE)) {
    std::fwprintf(stderr, L"configure_failed data_directory=\"%ls\" operation=\"CopyFile(password)\" error=%lu\n",
                  data_directory.c_str(), GetLastError());
    return ERROR_WRITE_FAULT;
  }
  const std::wstring ini_contents =
      L"[Server]\r\n"
      L"AllowInsecureHttp=1\r\n"
      L"Port=" + port_text + L"\r\n"
      L"EnrollmentTokenFile=" + token_destination + L"\r\n"
      L"AdminPasswordFile=" + password_destination + L"\r\n";
  if (!usb_dlp::write_utf16_text_file(
          join_path(data_directory, L"legacy-server.ini"), ini_contents, &error)) {
    std::fwprintf(stderr, L"configure_failed data_directory=\"%ls\" error=\"%ls\"\n",
                  data_directory.c_str(), error.c_str());
    return ERROR_WRITE_FAULT;
  }
  std::fwprintf(stdout, L"configure_succeeded data_directory=\"%ls\"\n", data_directory.c_str());
  return 0;
}

bool parse_http_request(SOCKET socket, HttpRequest* request) {
  std::string data;
  char buffer[4096];
  std::size_t headers_end = std::string::npos;
  while ((headers_end = data.find("\r\n\r\n")) == std::string::npos) {
    const int read = recv(socket, buffer, sizeof(buffer), 0);
    if (read <= 0 || data.size() + read > 64U * 1024U) return false;
    data.append(buffer, static_cast<std::size_t>(read));
  }
  const std::string headers = data.substr(0, headers_end);
  const std::string::size_type line_end = headers.find("\r\n");
  const std::string first = headers.substr(0, line_end);
  const std::string::size_type first_space = first.find(' ');
  const std::string::size_type second_space = first.find(' ', first_space + 1);
  if (first_space == std::string::npos || second_space == std::string::npos) return false;
  request->method = first.substr(0, first_space);
  request->path = first.substr(first_space + 1, second_space - first_space - 1);
  std::size_t cursor = line_end == std::string::npos ? headers.size() : line_end + 2;
  while (cursor < headers.size()) {
    const std::size_t end = headers.find("\r\n", cursor);
    const std::string line = headers.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) request->headers[lower_ascii(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    if (end == std::string::npos) break;
    cursor = end + 2;
  }
  std::size_t content_length = 0;
  const std::map<std::string, std::string>::const_iterator found = request->headers.find("content-length");
  if (found != request->headers.end()) content_length = static_cast<std::size_t>(std::strtoul(found->second.c_str(), nullptr, 10));
  if (content_length > 1024U * 1024U) return false;
  request->body = data.substr(headers_end + 4);
  while (request->body.size() < content_length) {
    const int read = recv(socket, buffer, sizeof(buffer), 0);
    if (read <= 0) return false;
    request->body.append(buffer, static_cast<std::size_t>(read));
  }
  if (request->body.size() > content_length) request->body.resize(content_length);
  return true;
}

std::string bearer(const HttpRequest& request) {
  const std::map<std::string, std::string>::const_iterator found = request.headers.find("authorization");
  if (found == request.headers.end() || found->second.size() < 8 ||
      lower_ascii(found->second.substr(0, 7)) != "bearer ") return std::string();
  return found->second.substr(7);
}

std::string authorization(const HttpRequest& request) {
  const std::map<std::string, std::string>::const_iterator found = request.headers.find("authorization");
  return found == request.headers.end() ? std::string() : found->second;
}

HttpResult route(StateStore* store, const HttpRequest& request) {
  if (request.method == "POST" && request.path == "/api/v1/agent/enroll") return store->enroll(request.body);
  if (request.method == "GET" && request.path == "/api/v1/agent/policy") return store->policy(bearer(request));
  if (request.method == "POST" && request.path == "/api/v1/agent/heartbeat") return store->heartbeat(bearer(request));
  if (request.method == "POST" && request.path == "/api/v1/agent/requests") return store->create_request(bearer(request), request.body);
  if (request.method == "GET" && request.path == "/api/v1/admin/requests") {
    return store->admin_requests(authorization(request));
  }
  if (request.method == "GET" && request.path == "/api/v1/admin/workstations") {
    return store->admin_workstations(authorization(request));
  }
  const std::string prefix = "/api/v1/admin/requests/";
  if (request.method == "POST" && request.path.find(prefix) == 0) {
    const std::string remainder = request.path.substr(prefix.size());
    const std::string::size_type separator = remainder.find('/');
    if (separator != std::string::npos) {
      const std::string id = remainder.substr(0, separator);
      const std::string action = remainder.substr(separator + 1);
      if (action == "approve") {
        int days = 30;
        usb_dlp::JsonValue body;
        std::string parse_error;
        if (usb_dlp::parse_json(request.body, &body, &parse_error)) {
          const usb_dlp::JsonValue* requested_days = body.find("durationDays");
          if (requested_days != nullptr && requested_days->type() == usb_dlp::JsonValue::Type::number) {
            days = static_cast<int>(requested_days->number_value());
          }
        }
        if (days != 1 && days != 7 && days != 30 && days != 90) {
          usb_dlp::JsonValue error = usb_dlp::JsonValue::object();
          error.object_items()["error"] = usb_dlp::JsonValue(std::string("invalid_duration"));
          return HttpResult{422, usb_dlp::write_json(error)};
        }
        return store->admin_decide(authorization(request), id, "approved", days);
      }
      if (action == "deny") return store->admin_decide(authorization(request), id, "denied", 0);
      if (action == "revoke") return store->admin_decide(authorization(request), id, "revoked", 0);
    }
  }
  usb_dlp::JsonValue error = usb_dlp::JsonValue::object();
  error.object_items()["error"] = usb_dlp::JsonValue(std::string("not_found"));
  return HttpResult{404, usb_dlp::write_json(error)};
}

void send_http(SOCKET socket, const HttpResult& result) {
  const char* reason = result.status == 200 ? "OK" : result.status == 201 ? "Created" :
                       result.status == 400 ? "Bad Request" : result.status == 401 ? "Unauthorized" :
                       result.status == 403 ? "Forbidden" : result.status == 404 ? "Not Found" :
                       result.status == 422 ? "Unprocessable Entity" : "Internal Server Error";
  const std::string response = "HTTP/1.1 " + std::to_string(result.status) + " " + reason +
      "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
      std::to_string(result.body.size()) + "\r\n\r\n" + result.body;
  send(socket, response.data(), static_cast<int>(response.size()), 0);
}

int serve(StateStore* store, unsigned short port, HANDLE stop_event) {
  WSADATA data = {};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return ERROR_NETWORK_UNREACHABLE;
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) { WSACleanup(); return ERROR_OPEN_FAILED; }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
      listen(listener, SOMAXCONN) == SOCKET_ERROR) {
    closesocket(listener); WSACleanup(); return ERROR_OPEN_FAILED;
  }
  u_long nonblocking = 1;
  ioctlsocket(listener, FIONBIO, &nonblocking);
  while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
    SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) { Sleep(100); continue; }
    DWORD timeout = 20000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    HttpRequest request;
    if (parse_http_request(client, &request)) send_http(client, route(store, request));
    shutdown(client, SD_BOTH);
    closesocket(client);
  }
  closesocket(listener);
  WSACleanup();
  return 0;
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

bool initialize_store(StateStore** store, std::wstring* data_directory, std::wstring* error) {
  wchar_t override_directory[32768] = {};
  const DWORD override_length = GetEnvironmentVariableW(
      L"USB_DLP_LEGACY_DATA_DIRECTORY", override_directory,
      static_cast<DWORD>(sizeof(override_directory) / sizeof(override_directory[0])));
  *data_directory = override_length > 0 && override_length <
      sizeof(override_directory) / sizeof(override_directory[0])
          ? std::wstring(override_directory, override_length)
          : join_path(usb_dlp::common_application_data_path(), L"UsbDlp");
  wchar_t test_mode[64] = {};
  GetEnvironmentVariableW(L"USB_DLP_LEGACY_TEST_MODE", test_mode,
                          static_cast<DWORD>(sizeof(test_mode) / sizeof(test_mode[0])));
  const bool explicit_unprotected_test = override_length > 0 &&
      std::wstring(test_mode) == L"I_ACCEPT_UNPROTECTED_TEST_DATA";
  if (!explicit_unprotected_test &&
      !usb_dlp::ensure_protected_directory(*data_directory, error)) return false;
  const std::wstring ini = join_path(*data_directory, L"legacy-server.ini");
  wchar_t token_path[2048] = {};
  const std::wstring default_token = join_path(*data_directory, L"enrollment-token.txt");
  GetPrivateProfileStringW(L"Server", L"EnrollmentTokenFile", default_token.c_str(), token_path,
                           static_cast<DWORD>(sizeof(token_path) / sizeof(token_path[0])), ini.c_str());
  std::string token;
  if (!read_small_file(token_path, &token) || token.size() < 32) {
    if (error != nullptr) *error = L"EnrollmentTokenFile is missing or shorter than 32 characters.";
    return false;
  }
  wchar_t password_path[2048] = {};
  const std::wstring default_password = join_path(*data_directory, L"admin-password.txt");
  GetPrivateProfileStringW(L"Server", L"AdminPasswordFile", default_password.c_str(), password_path,
                           static_cast<DWORD>(sizeof(password_path) / sizeof(password_path[0])), ini.c_str());
  std::string password;
  if (!read_small_file(password_path, &password) || password.size() < 14) {
    SecureZeroMemory(&token[0], token.size());
    if (error != nullptr) *error = L"AdminPasswordFile is missing or shorter than 14 characters.";
    return false;
  }
  *store = new StateStore(*data_directory, token, password);
  SecureZeroMemory(&token[0], token.size());
  SecureZeroMemory(&password[0], password.size());
  return (*store)->load(error);
}

int run_server(HANDLE stop_event) {
  StateStore* store = nullptr;
  std::wstring data_directory;
  std::wstring error;
  if (!initialize_store(&store, &data_directory, &error)) return ERROR_INVALID_DATA;
  const std::wstring ini = join_path(data_directory, L"legacy-server.ini");
  if (GetPrivateProfileIntW(L"Server", L"AllowInsecureHttp", 0, ini.c_str()) == 0) {
    delete store;
    return ERROR_ACCESS_DISABLED_BY_POLICY;
  }
  UINT configured_port = GetPrivateProfileIntW(L"Server", L"Port", 5080, ini.c_str());
  if (configured_port < 1 || configured_port > 65535) configured_port = 5080;
  const int result = serve(store, static_cast<unsigned short>(configured_port), stop_event);
  delete store;
  return result;
}

void WINAPI service_main(DWORD, wchar_t**) {
  g_status_handle = RegisterServiceCtrlHandlerW(kServiceName, control_handler);
  if (g_status_handle == nullptr) return;
  report_status(SERVICE_START_PENDING, NO_ERROR, 5000);
  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_stop_event == nullptr) { report_status(SERVICE_STOPPED, GetLastError(), 0); return; }
  report_status(SERVICE_RUNNING, NO_ERROR, 0);
  const int result = run_server(g_stop_event);
  CloseHandle(g_stop_event);
  g_stop_event = nullptr;
  report_status(SERVICE_STOPPED, static_cast<DWORD>(result), 0);
}

int run_admin(int argc, wchar_t** argv) {
  StateStore* store = nullptr;
  std::wstring directory;
  std::wstring error;
  if (!initialize_store(&store, &directory, &error)) {
    std::wcerr << error << L'\n';
    return 2;
  }
  bool success = false;
  const std::wstring command = argv[1];
  if (command == L"--requests") success = store->list_requests();
  else if (command == L"--workstations") success = store->list_workstations(argc >= 3 ? usb_dlp::wide_to_utf8(argv[2]) : std::string());
  else if (command == L"--approve" && argc >= 3) success = store->decide(usb_dlp::wide_to_utf8(argv[2]), "approved", argc >= 4 ? _wtoi(argv[3]) : 30);
  else if (command == L"--deny" && argc >= 3) success = store->decide(usb_dlp::wide_to_utf8(argv[2]), "denied", 0);
  else if (command == L"--revoke" && argc >= 3) success = store->revoke(usb_dlp::wide_to_utf8(argv[2]));
  delete store;
  if (!success) std::wcerr << L"Command failed or the requested object is not in the required state.\n";
  return success ? 0 : 3;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 5 && std::wstring(argv[1]) == L"--configure") {
    return configure_server(argv[2], argv[3], argv[4]);
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
  if (argc >= 2 && std::wstring(argv[1]) != L"--console") return run_admin(argc, argv);
  if (argc == 2) {
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) return static_cast<int>(GetLastError());
    const int result = run_server(stop_event);
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
