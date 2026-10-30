#include "usb_dlp/policy.h"

#include <algorithm>
#include <cctype>

namespace usb_dlp {
namespace {

const char* const kSafetyClasses[] = {
    "{745a17a0-74d3-11d0-b6fe-00a0c90f57da}",
    "{4d36e96b-e325-11ce-bfc1-08002be10318}",
    "{4d36e96f-e325-11ce-bfc1-08002be10318}",
    "{50dd5230-ba8a-11d1-bf5d-0000f805f530}",
    "{4d36e979-e325-11ce-bfc1-08002be10318}",
    "{36fc9e60-c465-11cf-8056-444553540000}",
    "{4d36e96c-e325-11ce-bfc1-08002be10318}",
    "{c166523c-fe0c-4a94-a586-f1a80cfbbf3e}",
    "{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}",
    "{4d36e972-e325-11ce-bfc1-08002be10318}",
    "{4d36e97d-e325-11ce-bfc1-08002be10318}",
    "{6bdd1fc6-810f-11d0-bec7-08002be2092f}",
    "{ca3e7ab9-b4c3-4ae6-8251-579ef933890f}",
};

std::string lower_ascii(const std::string& value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

bool equal_ignore_case(const std::string& left, const std::string& right) {
  return lower_ascii(left) == lower_ascii(right);
}

int specificity(MatchType type) {
  switch (type) {
    case MatchType::instance_id: return 4;
    case MatchType::vid_pid_serial: return 3;
    case MatchType::vid_pid: return 2;
    case MatchType::device_class: return 1;
  }
  return 0;
}

int scope_precedence(ScopeType type) {
  switch (type) {
    case ScopeType::workstation: return 3;
    case ScopeType::group: return 2;
    case ScopeType::global: return 1;
  }
  return 0;
}

bool matches(const DeviceInfo& device, const DeviceEntry& entry) {
  switch (entry.match_type) {
    case MatchType::instance_id:
      return equal_ignore_case(device.instance_id, entry.instance_id);
    case MatchType::vid_pid_serial:
      return device.has_serial && equal_ignore_case(device.vid, entry.vid) &&
             equal_ignore_case(device.pid, entry.pid) &&
             equal_ignore_case(device.serial, entry.serial);
    case MatchType::vid_pid:
      return equal_ignore_case(device.vid, entry.vid) && equal_ignore_case(device.pid, entry.pid);
    case MatchType::device_class:
      return device.device_class == entry.device_class;
  }
  return false;
}

bool better_entry(const DeviceEntry* candidate, const DeviceEntry* current) {
  if (current == nullptr) return true;
  const int candidate_specificity = specificity(candidate->match_type);
  const int current_specificity = specificity(current->match_type);
  if (candidate_specificity != current_specificity) {
    return candidate_specificity > current_specificity;
  }
  if (candidate->action != current->action) {
    return candidate->action == EntryAction::block;
  }
  return lower_ascii(candidate->id) < lower_ascii(current->id);
}

PolicyEvaluation result(
    Decision decision,
    const DeviceEntry* entry,
    const char* reason,
    const CompiledPolicy& policy,
    std::int64_t now_utc_ms) {
  PolicyEvaluation evaluation;
  evaluation.decision = decision;
  evaluation.matched_entry_id = entry == nullptr ? std::string() : entry->id;
  evaluation.reason_code = reason;
  evaluation.policy_version = policy.policy_version;
  evaluation.evaluated_at_utc_ms = now_utc_ms;
  return evaluation;
}

bool fail(PolicyError* error, const char* message) {
  if (error != nullptr) *error = PolicyError(message);
  return false;
}

}  // namespace

bool is_safety_class(const std::string& class_guid) {
  for (const char* safety_class : kSafetyClasses) {
    if (equal_ignore_case(class_guid, safety_class)) return true;
  }
  return false;
}

bool validate_policy(const CompiledPolicy& policy, PolicyError* error) {
  if (policy.schema_version != 1) return fail(error, "policy.schema_version");
  for (const DeviceEntry& entry : policy.entries) {
    if (entry.id.empty()) return fail(error, "entry.id");
    if (entry.mandatory &&
        (entry.scope_type != ScopeType::global || entry.action != EntryAction::block)) {
      return fail(error, "entry.mandatory");
    }
    if (entry.scope_type == ScopeType::global && !entry.scope_id.empty()) {
      return fail(error, "entry.scope_id");
    }
    if (entry.scope_type != ScopeType::global && entry.scope_id.empty()) {
      return fail(error, "entry.scope_id");
    }
    switch (entry.match_type) {
      case MatchType::instance_id:
        if (entry.instance_id.empty() || !entry.vid.empty() || !entry.pid.empty() ||
            !entry.serial.empty()) return fail(error, "entry.match_fields");
        break;
      case MatchType::vid_pid_serial:
        if (entry.vid.empty() || entry.pid.empty() || entry.serial.empty() ||
            !entry.instance_id.empty()) return fail(error, "entry.match_fields");
        break;
      case MatchType::vid_pid:
        if (entry.vid.empty() || entry.pid.empty() || !entry.serial.empty() ||
            !entry.instance_id.empty()) return fail(error, "entry.match_fields");
        break;
      case MatchType::device_class:
        if (!entry.vid.empty() || !entry.pid.empty() || !entry.serial.empty() ||
            !entry.instance_id.empty()) return fail(error, "entry.match_fields");
        break;
    }
  }
  return true;
}

PolicyEvaluation evaluate_policy(
    const DeviceInfo& device,
    const CompiledPolicy& policy,
    std::int64_t now_utc_ms,
    PolicyError* error) {
  if (!validate_policy(policy, error)) {
    return result(Decision::block, nullptr, "invalid_policy", policy, now_utc_ms);
  }

  if (is_safety_class(device.class_guid)) {
    return result(Decision::allow, nullptr, "safety_allowlist", policy, now_utc_ms);
  }

  std::vector<const DeviceEntry*> active_matches;
  for (const DeviceEntry& entry : policy.entries) {
    if (entry.has_expiry && entry.expires_at_utc_ms <= now_utc_ms) continue;
    if (matches(device, entry)) active_matches.push_back(&entry);
  }

  const DeviceEntry* mandatory = nullptr;
  for (const DeviceEntry* entry : active_matches) {
    if (entry->mandatory && better_entry(entry, mandatory)) mandatory = entry;
  }
  if (mandatory != nullptr) {
    return result(Decision::block, mandatory, "mandatory_global_block", policy, now_utc_ms);
  }

  for (int scope = 3; scope >= 1; --scope) {
    const DeviceEntry* selected = nullptr;
    for (const DeviceEntry* entry : active_matches) {
      if (scope_precedence(entry->scope_type) == scope && better_entry(entry, selected)) {
        selected = entry;
      }
    }
    if (selected != nullptr) {
      return selected->action == EntryAction::block
                 ? result(Decision::block, selected, "entry_block", policy, now_utc_ms)
                 : result(Decision::allow, selected, "entry_allow", policy, now_utc_ms);
    }
  }

  return policy.group_mode == GroupMode::default_deny
             ? result(Decision::block, nullptr, "default_deny", policy, now_utc_ms)
             : result(Decision::allow, nullptr, "default_allow", policy, now_utc_ms);
}

}  // namespace usb_dlp
