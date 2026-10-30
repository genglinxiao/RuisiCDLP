#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace usb_dlp {

class JsonValue {
 public:
  enum class Type { null_value, boolean, number, string, array, object };

  JsonValue();
  explicit JsonValue(bool value);
  explicit JsonValue(std::int64_t value);
  explicit JsonValue(const std::string& value);

  static JsonValue array();
  static JsonValue object();

  Type type() const { return type_; }
  bool boolean_value() const { return boolean_; }
  std::int64_t number_value() const { return number_; }
  const std::string& string_value() const { return string_; }
  const std::vector<JsonValue>& array_items() const { return array_; }
  std::vector<JsonValue>& array_items() { return array_; }
  const std::map<std::string, JsonValue>& object_items() const { return object_; }
  std::map<std::string, JsonValue>& object_items() { return object_; }
  const JsonValue* find(const std::string& name) const;

 private:
  Type type_;
  bool boolean_;
  std::int64_t number_;
  std::string string_;
  std::vector<JsonValue> array_;
  std::map<std::string, JsonValue> object_;
};

bool parse_json(const std::string& input, JsonValue* value, std::string* error);
std::string write_json(const JsonValue& value);

}  // namespace usb_dlp
