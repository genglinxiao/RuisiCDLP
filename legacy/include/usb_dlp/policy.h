#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usb_dlp {

enum class MatchType { instance_id, vid_pid_serial, vid_pid, device_class };
enum class EntryAction { allow, block };
enum class GroupMode { default_deny, default_allow };
enum class ScopeType { global, group, workstation };
enum class Decision { allow, block };
enum class ProductDeviceClass { usb_storage, wpd };

struct DeviceInfo {
  std::string instance_id;
  std::string vid;
  std::string pid;
  std::string serial;
  bool has_serial = false;
  ProductDeviceClass device_class = ProductDeviceClass::usb_storage;
  std::string class_guid;
  std::vector<std::string> ancestry_class_guids;
};

struct DeviceEntry {
  std::string id;
  MatchType match_type = MatchType::instance_id;
  std::string vid;
  std::string pid;
  std::string serial;
  std::string instance_id;
  ProductDeviceClass device_class = ProductDeviceClass::usb_storage;
  EntryAction action = EntryAction::block;
  bool mandatory = false;
  ScopeType scope_type = ScopeType::global;
  std::string scope_id;
  bool has_expiry = false;
  std::int64_t expires_at_utc_ms = 0;
};

struct CompiledPolicy {
  int schema_version = 1;
  std::int64_t policy_version = 0;
  GroupMode group_mode = GroupMode::default_deny;
  std::vector<DeviceEntry> entries;
};

struct PolicyEvaluation {
  Decision decision = Decision::block;
  std::string matched_entry_id;
  std::string reason_code;
  std::int64_t policy_version = 0;
  std::int64_t evaluated_at_utc_ms = 0;
};

class PolicyError {
 public:
  explicit PolicyError(const std::string& message) : message_(message) {}
  const std::string& message() const { return message_; }

 private:
  std::string message_;
};

bool validate_policy(const CompiledPolicy& policy, PolicyError* error);
PolicyEvaluation evaluate_policy(
    const DeviceInfo& device,
    const CompiledPolicy& policy,
    std::int64_t now_utc_ms,
    PolicyError* error);
bool is_safety_class(const std::string& class_guid);

}  // namespace usb_dlp
