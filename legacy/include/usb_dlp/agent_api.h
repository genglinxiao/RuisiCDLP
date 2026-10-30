#pragma once

#ifdef _WIN32

#include "usb_dlp/http_client.h"
#include "usb_dlp/policy.h"
#include "usb_dlp/windows_device.h"

#include <string>

namespace usb_dlp {

struct AgentIdentity {
  std::string workstation_id;
  std::string api_credential;
};

struct AccessRequestResult {
  std::string request_id;
  std::string status;
  std::string retry_after_utc;
};

class LegacyAgentApi {
 public:
  LegacyAgentApi(const std::wstring& server_url, bool allow_insecure_http)
      : client_(server_url, allow_insecure_http) {}

  bool valid() const { return client_.valid(); }
  const std::wstring& validation_error() const { return client_.validation_error(); }
  bool enroll(
      const std::string& enrollment_token,
      const std::string& implementation_profile,
      AgentIdentity* identity,
      std::wstring* error) const;
  bool fetch_policy(
      const AgentIdentity& identity,
      CompiledPolicy* policy,
      std::string* raw_json,
      std::wstring* error) const;
  bool heartbeat(
      const AgentIdentity& identity,
      std::int64_t policy_version,
      int queue_depth,
      std::wstring* error) const;
  bool request_access(
      const AgentIdentity& identity,
      const WindowsDevice& device,
      const std::string& client_request_id,
      const std::string& user_sid,
      const std::string& username,
      int requested_duration_days,
      const std::string& justification,
      AccessRequestResult* result,
      std::wstring* error) const;

 private:
  HttpClient client_;
};

bool encode_identity(const AgentIdentity& identity, std::string* json);
bool decode_identity(const std::string& json, AgentIdentity* identity);

}  // namespace usb_dlp

#endif
