#include "usb_dlp/policy.h"
#include "usb_dlp/policy_json.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

const std::int64_t kNow = 1787054400000LL;

void require(bool condition, const char* name) {
  if (!condition) {
    std::cerr << "FAILED: " << name << std::endl;
    std::exit(1);
  }
}

usb_dlp::DeviceInfo device() {
  usb_dlp::DeviceInfo value;
  value.instance_id = "USB\\VID_0781&PID_5583\\4C530001230904";
  value.vid = "0781";
  value.pid = "5583";
  value.serial = "4C530001230904";
  value.has_serial = true;
  value.device_class = usb_dlp::ProductDeviceClass::usb_storage;
  value.class_guid = "{4d36e967-e325-11ce-bfc1-08002be10318}";
  return value;
}

usb_dlp::DeviceEntry entry(
    const char* id,
    usb_dlp::MatchType match_type,
    usb_dlp::EntryAction action) {
  usb_dlp::DeviceEntry value;
  value.id = id;
  value.match_type = match_type;
  value.action = action;
  value.scope_type = usb_dlp::ScopeType::global;
  switch (match_type) {
    case usb_dlp::MatchType::instance_id:
      value.instance_id = device().instance_id;
      break;
    case usb_dlp::MatchType::vid_pid_serial:
      value.vid = "0781";
      value.pid = "5583";
      value.serial = "4C530001230904";
      break;
    case usb_dlp::MatchType::vid_pid:
      value.vid = "0781";
      value.pid = "5583";
      break;
    case usb_dlp::MatchType::device_class:
      value.device_class = usb_dlp::ProductDeviceClass::usb_storage;
      break;
  }
  return value;
}

usb_dlp::CompiledPolicy policy(usb_dlp::GroupMode mode) {
  usb_dlp::CompiledPolicy value;
  value.schema_version = 1;
  value.policy_version = 42;
  value.group_mode = mode;
  return value;
}

usb_dlp::PolicyEvaluation evaluate(
    const usb_dlp::DeviceInfo& input,
    const usb_dlp::CompiledPolicy& compiled) {
  usb_dlp::PolicyError error("");
  return usb_dlp::evaluate_policy(input, compiled, kNow, &error);
}

void safety_allowlist_wins() {
  usb_dlp::DeviceInfo input = device();
  input.class_guid = "{4D36E96B-E325-11CE-BFC1-08002BE10318}";
  usb_dlp::CompiledPolicy compiled = policy(usb_dlp::GroupMode::default_deny);
  compiled.entries.push_back(entry("1", usb_dlp::MatchType::device_class, usb_dlp::EntryAction::block));
  require(evaluate(input, compiled).decision == usb_dlp::Decision::allow, "T-M1-01");
}

void safety_ancestor_does_not_allow_storage_disk() {
  usb_dlp::DeviceInfo input = device();
  input.instance_id = "USBSTOR\\DISK&VEN_SAMSUNG&PROD_FLASH_DRIVE\\0356019020000613&0";
  input.ancestry_class_guids.push_back("{36FC9E60-C465-11CF-8056-444553540000}");
  const usb_dlp::PolicyEvaluation result =
      evaluate(input, policy(usb_dlp::GroupMode::default_deny));
  require(result.decision == usb_dlp::Decision::block && result.reason_code == "default_deny",
          "T-M1-01-ancestor-hub-does-not-exempt-disk");
}

void specificity_and_block_tie() {
  usb_dlp::CompiledPolicy compiled = policy(usb_dlp::GroupMode::default_deny);
  compiled.entries.push_back(entry("2", usb_dlp::MatchType::vid_pid, usb_dlp::EntryAction::block));
  compiled.entries.push_back(entry("1", usb_dlp::MatchType::instance_id, usb_dlp::EntryAction::allow));
  require(evaluate(device(), compiled).decision == usb_dlp::Decision::allow, "T-M1-02");

  compiled.entries.clear();
  compiled.entries.push_back(entry("1", usb_dlp::MatchType::vid_pid, usb_dlp::EntryAction::allow));
  compiled.entries.push_back(entry("2", usb_dlp::MatchType::vid_pid, usb_dlp::EntryAction::block));
  require(evaluate(device(), compiled).decision == usb_dlp::Decision::block, "T-M1-03");
}

void expiry_and_defaults() {
  usb_dlp::CompiledPolicy compiled = policy(usb_dlp::GroupMode::default_allow);
  usb_dlp::DeviceEntry expired = entry("1", usb_dlp::MatchType::vid_pid, usb_dlp::EntryAction::block);
  expired.has_expiry = true;
  expired.expires_at_utc_ms = kNow;
  compiled.entries.push_back(expired);
  require(evaluate(device(), compiled).reason_code == "default_allow", "T-M1-04/T-M1-09");

  require(evaluate(device(), policy(usb_dlp::GroupMode::default_deny)).decision ==
              usb_dlp::Decision::block,
          "T-M1-06");
  require(evaluate(device(), policy(usb_dlp::GroupMode::default_allow)).decision ==
              usb_dlp::Decision::allow,
          "T-M1-07");
}

void wildcard_and_case() {
  usb_dlp::CompiledPolicy compiled = policy(usb_dlp::GroupMode::default_allow);
  usb_dlp::DeviceEntry block = entry("1", usb_dlp::MatchType::vid_pid, usb_dlp::EntryAction::block);
  block.vid = "aBcD";
  block.pid = "eF01";
  compiled.entries.push_back(block);
  usb_dlp::DeviceInfo input = device();
  input.vid = "ABCD";
  input.pid = "EF01";
  input.has_serial = false;
  input.serial.clear();
  require(evaluate(input, compiled).decision == usb_dlp::Decision::block, "T-M1-05/T-M1-08");
}

void order_scope_and_mandatory() {
  usb_dlp::CompiledPolicy compiled = policy(usb_dlp::GroupMode::default_deny);
  usb_dlp::DeviceEntry global = entry("2", usb_dlp::MatchType::instance_id, usb_dlp::EntryAction::block);
  usb_dlp::DeviceEntry workstation = entry("1", usb_dlp::MatchType::device_class, usb_dlp::EntryAction::allow);
  workstation.scope_type = usb_dlp::ScopeType::workstation;
  workstation.scope_id = "00000000-0000-0000-0000-000000000101";
  compiled.entries.push_back(global);
  compiled.entries.push_back(workstation);
  require(evaluate(device(), compiled).matched_entry_id == "1", "T-M1-10/T-M1-12");

  global.mandatory = true;
  compiled.entries[0] = global;
  const usb_dlp::PolicyEvaluation result = evaluate(device(), compiled);
  require(result.decision == usb_dlp::Decision::block &&
              result.reason_code == "mandatory_global_block",
          "T-M1-12-mandatory");
}

void malformed_and_metadata() {
  usb_dlp::CompiledPolicy compiled = policy(usb_dlp::GroupMode::default_deny);
  usb_dlp::DeviceEntry malformed = entry("1", usb_dlp::MatchType::vid_pid, usb_dlp::EntryAction::allow);
  malformed.pid.clear();
  compiled.entries.push_back(malformed);
  usb_dlp::PolicyError error("");
  usb_dlp::evaluate_policy(device(), compiled, kNow, &error);
  require(error.message() == "entry.match_fields", "T-M1-11");

  compiled = policy(usb_dlp::GroupMode::default_allow);
  compiled.entries.push_back(entry("00000000-0000-0000-0000-000000000001",
                                   usb_dlp::MatchType::vid_pid,
                                   usb_dlp::EntryAction::block));
  const usb_dlp::PolicyEvaluation result = evaluate(device(), compiled);
  require(result.policy_version == 42 && result.evaluated_at_utc_ms == kNow &&
              result.reason_code == "entry_block",
          "T-M1-13");
}

void canonical_json_policy_decodes() {
  const std::string json =
      "{\"schemaVersion\":1,\"policyVersion\":42,"
      "\"generatedAtUtc\":\"2026-08-18T12:00:00Z\","
      "\"workstationId\":\"00000000-0000-0000-0000-000000000101\","
      "\"groupId\":\"00000000-0000-0000-0000-000000000201\","
      "\"groupMode\":\"default_deny\",\"entries\":[{"
      "\"id\":\"00000000-0000-0000-0000-000000000001\","
      "\"matchType\":\"vid_pid\",\"vid\":\"0781\",\"pid\":\"5583\","
      "\"serial\":null,\"instanceId\":null,\"deviceClass\":null,"
      "\"displayName\":\"Block test\",\"action\":\"block\",\"mandatory\":false,"
      "\"scopeType\":\"global\",\"scopeId\":null,\"expiresAtUtc\":null,"
      "\"note\":null,\"deviceInstallIds\":[]}]}";
  usb_dlp::CompiledPolicy decoded;
  std::string error;
  require(usb_dlp::decode_compiled_policy(json, &decoded, &error), "T-M10-01-json-decode");
  require(evaluate(device(), decoded).decision == usb_dlp::Decision::block,
          "T-M10-01-json-evaluate");

  const std::string duplicate =
      "{\"schemaVersion\":1,\"schemaVersion\":1,\"policyVersion\":1,"
      "\"groupMode\":\"default_deny\",\"entries\":[]}";
  require(!usb_dlp::decode_compiled_policy(duplicate, &decoded, &error),
          "T-M10-01-json-duplicate-rejected");
}

}  // namespace

int main() {
  safety_allowlist_wins();
  safety_ancestor_does_not_allow_storage_disk();
  specificity_and_block_tie();
  expiry_and_defaults();
  wildcard_and_case();
  order_scope_and_mandatory();
  malformed_and_metadata();
  canonical_json_policy_decodes();
  std::cout << "Native policy parity tests passed." << std::endl;
  return 0;
}
