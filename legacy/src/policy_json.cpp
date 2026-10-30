#include "usb_dlp/policy_json.h"

#include "usb_dlp/json.h"

#include <cctype>
#include <cstdlib>

namespace usb_dlp {
namespace {

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

const JsonValue* required(
    const JsonValue& object,
    const char* name,
    JsonValue::Type type,
    std::string* error) {
  const JsonValue* value = object.find(name);
  if (value == nullptr || value->type() != type) {
    if (error != nullptr) *error = std::string("policy.") + name;
    return nullptr;
  }
  return value;
}

std::string nullable_string(const JsonValue& object, const char* name, bool* valid) {
  const JsonValue* value = object.find(name);
  if (value == nullptr || value->type() == JsonValue::Type::null_value) return std::string();
  if (value->type() != JsonValue::Type::string) {
    *valid = false;
    return std::string();
  }
  return value->string_value();
}

bool parse_match_type(const std::string& value, MatchType* output) {
  if (value == "instance_id") *output = MatchType::instance_id;
  else if (value == "vid_pid_serial") *output = MatchType::vid_pid_serial;
  else if (value == "vid_pid") *output = MatchType::vid_pid;
  else if (value == "device_class") *output = MatchType::device_class;
  else return false;
  return true;
}

bool parse_action(const std::string& value, EntryAction* output) {
  if (value == "allow") *output = EntryAction::allow;
  else if (value == "block") *output = EntryAction::block;
  else return false;
  return true;
}

bool parse_scope(const std::string& value, ScopeType* output) {
  if (value == "global") *output = ScopeType::global;
  else if (value == "group") *output = ScopeType::group;
  else if (value == "workstation") *output = ScopeType::workstation;
  else return false;
  return true;
}

bool parse_device_class(const std::string& value, ProductDeviceClass* output) {
  if (value == "usb_storage") *output = ProductDeviceClass::usb_storage;
  else if (value == "wpd") *output = ProductDeviceClass::wpd;
  else return false;
  return true;
}

bool fixed_digits(const std::string& value, std::size_t position, std::size_t count, int* output) {
  if (position + count > value.size()) return false;
  int result = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const char character = value[position + index];
    if (!std::isdigit(static_cast<unsigned char>(character))) return false;
    result = (result * 10) + (character - '0');
  }
  *output = result;
  return true;
}

std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
  const unsigned day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
  const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<std::int64_t>(era) * 146097 + static_cast<int>(day_of_era) - 719468;
}

bool parse_utc_ms(const std::string& value, std::int64_t* output) {
  if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value[value.size() - 1] != 'Z') return false;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!fixed_digits(value, 0, 4, &year) || !fixed_digits(value, 5, 2, &month) ||
      !fixed_digits(value, 8, 2, &day) || !fixed_digits(value, 11, 2, &hour) ||
      !fixed_digits(value, 14, 2, &minute) || !fixed_digits(value, 17, 2, &second)) return false;
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;
  int milliseconds = 0;
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    const std::size_t digits = value.size() - 21;
    if (digits < 1 || digits > 7) return false;
    int fraction = 0;
    if (!fixed_digits(value, 20, digits, &fraction)) return false;
    if (digits == 1) milliseconds = fraction * 100;
    else if (digits == 2) milliseconds = fraction * 10;
    else milliseconds = fraction;
    while (digits > 3 && milliseconds > 999) milliseconds /= 10;
  }
  *output = ((days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
              hour * 3600 + minute * 60 + second) * 1000) + milliseconds;
  return true;
}

bool decode_entry(const JsonValue& value, DeviceEntry* entry, std::string* error) {
  if (value.type() != JsonValue::Type::object) return fail(error, "policy.entry");
  const JsonValue* id = required(value, "id", JsonValue::Type::string, error);
  const JsonValue* match = required(value, "matchType", JsonValue::Type::string, error);
  const JsonValue* action = required(value, "action", JsonValue::Type::string, error);
  const JsonValue* mandatory = required(value, "mandatory", JsonValue::Type::boolean, error);
  const JsonValue* scope = required(value, "scopeType", JsonValue::Type::string, error);
  if (id == nullptr || match == nullptr || action == nullptr || mandatory == nullptr || scope == nullptr) return false;
  entry->id = id->string_value();
  if (!parse_match_type(match->string_value(), &entry->match_type) ||
      !parse_action(action->string_value(), &entry->action) ||
      !parse_scope(scope->string_value(), &entry->scope_type)) return fail(error, "policy.entry_enum");
  entry->mandatory = mandatory->boolean_value();
  bool valid = true;
  entry->vid = nullable_string(value, "vid", &valid);
  entry->pid = nullable_string(value, "pid", &valid);
  entry->serial = nullable_string(value, "serial", &valid);
  entry->instance_id = nullable_string(value, "instanceId", &valid);
  entry->scope_id = nullable_string(value, "scopeId", &valid);
  if (!valid) return fail(error, "policy.entry_string");
  const JsonValue* device_class = value.find("deviceClass");
  if (device_class != nullptr && device_class->type() != JsonValue::Type::null_value) {
    if (device_class->type() != JsonValue::Type::string ||
        !parse_device_class(device_class->string_value(), &entry->device_class)) {
      return fail(error, "policy.deviceClass");
    }
  }
  const JsonValue* expiry = value.find("expiresAtUtc");
  if (expiry != nullptr && expiry->type() != JsonValue::Type::null_value) {
    if (expiry->type() != JsonValue::Type::string ||
        !parse_utc_ms(expiry->string_value(), &entry->expires_at_utc_ms)) {
      return fail(error, "policy.expiresAtUtc");
    }
    entry->has_expiry = true;
  }
  return true;
}

}  // namespace

bool decode_compiled_policy(
    const std::string& json,
    CompiledPolicy* policy,
    std::string* error) {
  if (policy == nullptr) return false;
  JsonValue root;
  if (!parse_json(json, &root, error) || root.type() != JsonValue::Type::object) {
    return fail(error, "policy.json");
  }
  const JsonValue* schema = required(root, "schemaVersion", JsonValue::Type::number, error);
  const JsonValue* version = required(root, "policyVersion", JsonValue::Type::number, error);
  const JsonValue* mode = required(root, "groupMode", JsonValue::Type::string, error);
  const JsonValue* entries = required(root, "entries", JsonValue::Type::array, error);
  if (schema == nullptr || version == nullptr || mode == nullptr || entries == nullptr) return false;
  CompiledPolicy decoded;
  decoded.schema_version = static_cast<int>(schema->number_value());
  decoded.policy_version = version->number_value();
  if (mode->string_value() == "default_deny") decoded.group_mode = GroupMode::default_deny;
  else if (mode->string_value() == "default_allow") decoded.group_mode = GroupMode::default_allow;
  else return fail(error, "policy.groupMode");
  if (entries->array_items().size() > 10000) return fail(error, "policy.entries_limit");
  for (const JsonValue& item : entries->array_items()) {
    DeviceEntry entry;
    if (!decode_entry(item, &entry, error)) return false;
    decoded.entries.push_back(entry);
  }
  PolicyError validation_error("");
  if (!validate_policy(decoded, &validation_error)) {
    if (error != nullptr) *error = validation_error.message();
    return false;
  }
  *policy = decoded;
  return true;
}

}  // namespace usb_dlp
