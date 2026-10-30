#include "usb_dlp/json.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sstream>

namespace usb_dlp {

JsonValue::JsonValue() : type_(Type::null_value), boolean_(false), number_(0) {}
JsonValue::JsonValue(bool value) : type_(Type::boolean), boolean_(value), number_(0) {}
JsonValue::JsonValue(std::int64_t value) : type_(Type::number), boolean_(false), number_(value) {}
JsonValue::JsonValue(const std::string& value)
    : type_(Type::string), boolean_(false), number_(0), string_(value) {}

JsonValue JsonValue::array() {
  JsonValue value;
  value.type_ = Type::array;
  return value;
}

JsonValue JsonValue::object() {
  JsonValue value;
  value.type_ = Type::object;
  return value;
}

const JsonValue* JsonValue::find(const std::string& name) const {
  const std::map<std::string, JsonValue>::const_iterator found = object_.find(name);
  return found == object_.end() ? nullptr : &found->second;
}

namespace {

void append_utf8(unsigned int codepoint, std::string* output) {
  if (codepoint <= 0x7F) {
    output->push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    output->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    output->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    output->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    output->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

class Parser {
 public:
  explicit Parser(const std::string& input) : input_(input), position_(0), depth_(0) {}

  bool parse(JsonValue* value, std::string* error) {
    if (input_.size() > 1024U * 1024U) return fail(error, "json.too_large");
    skip_space();
    if (!parse_value(value, error)) return false;
    skip_space();
    return position_ == input_.size() || fail(error, "json.trailing_content");
  }

 private:
  const std::string& input_;
  std::size_t position_;
  int depth_;

  bool fail(std::string* error, const char* message) const {
    if (error != nullptr) *error = message;
    return false;
  }

  void skip_space() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\t' ||
            input_[position_] == '\r' || input_[position_] == '\n')) {
      ++position_;
    }
  }

  bool consume(char expected) {
    if (position_ >= input_.size() || input_[position_] != expected) return false;
    ++position_;
    return true;
  }

  bool parse_value(JsonValue* value, std::string* error) {
    if (position_ >= input_.size()) return fail(error, "json.unexpected_end");
    const char current = input_[position_];
    if (current == 'n') return literal("null", JsonValue(), value, error);
    if (current == 't') return literal("true", JsonValue(true), value, error);
    if (current == 'f') return literal("false", JsonValue(false), value, error);
    if (current == '"') {
      std::string parsed;
      if (!parse_string(&parsed, error)) return false;
      *value = JsonValue(parsed);
      return true;
    }
    if (current == '[') return parse_array(value, error);
    if (current == '{') return parse_object(value, error);
    if (current == '-' || (current >= '0' && current <= '9')) return parse_number(value, error);
    return fail(error, "json.unexpected_token");
  }

  bool literal(const char* text, const JsonValue& parsed, JsonValue* value, std::string* error) {
    const std::size_t length = std::char_traits<char>::length(text);
    if (input_.compare(position_, length, text) != 0) return fail(error, "json.invalid_literal");
    position_ += length;
    *value = parsed;
    return true;
  }

  int hex(char character) const {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return 10 + character - 'a';
    if (character >= 'A' && character <= 'F') return 10 + character - 'A';
    return -1;
  }

  bool unicode_escape(unsigned int* codepoint, std::string* error) {
    if (position_ + 4 > input_.size()) return fail(error, "json.invalid_unicode");
    unsigned int value = 0;
    for (int index = 0; index < 4; ++index) {
      const int digit = hex(input_[position_++]);
      if (digit < 0) return fail(error, "json.invalid_unicode");
      value = (value << 4) | static_cast<unsigned int>(digit);
    }
    *codepoint = value;
    return true;
  }

  bool parse_string(std::string* value, std::string* error) {
    if (!consume('"')) return fail(error, "json.expected_string");
    value->clear();
    while (position_ < input_.size()) {
      const unsigned char current = static_cast<unsigned char>(input_[position_++]);
      if (current == '"') return true;
      if (current < 0x20) return fail(error, "json.control_character");
      if (current != '\\') {
        value->push_back(static_cast<char>(current));
        continue;
      }
      if (position_ >= input_.size()) return fail(error, "json.invalid_escape");
      const char escaped = input_[position_++];
      switch (escaped) {
        case '"': value->push_back('"'); break;
        case '\\': value->push_back('\\'); break;
        case '/': value->push_back('/'); break;
        case 'b': value->push_back('\b'); break;
        case 'f': value->push_back('\f'); break;
        case 'n': value->push_back('\n'); break;
        case 'r': value->push_back('\r'); break;
        case 't': value->push_back('\t'); break;
        case 'u': {
          unsigned int codepoint = 0;
          if (!unicode_escape(&codepoint, error)) return false;
          if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (position_ + 2 > input_.size() || input_[position_] != '\\' || input_[position_ + 1] != 'u') {
              return fail(error, "json.invalid_surrogate");
            }
            position_ += 2;
            unsigned int low = 0;
            if (!unicode_escape(&low, error) || low < 0xDC00 || low > 0xDFFF) {
              return fail(error, "json.invalid_surrogate");
            }
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
          } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            return fail(error, "json.invalid_surrogate");
          }
          append_utf8(codepoint, value);
          break;
        }
        default: return fail(error, "json.invalid_escape");
      }
    }
    return fail(error, "json.unterminated_string");
  }

  bool parse_number(JsonValue* value, std::string* error) {
    const std::size_t start = position_;
    if (input_[position_] == '-') ++position_;
    if (position_ >= input_.size()) return fail(error, "json.invalid_number");
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) {
        return fail(error, "json.leading_zero");
      }
    } else {
      if (!std::isdigit(static_cast<unsigned char>(input_[position_]))) return fail(error, "json.invalid_number");
      while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
    }
    if (position_ < input_.size() && (input_[position_] == '.' || input_[position_] == 'e' || input_[position_] == 'E')) {
      return fail(error, "json.non_integer_number");
    }
    const std::string text = input_.substr(start, position_ - start);
    char* end = nullptr;
    errno = 0;
    const long long parsed = _strtoi64(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || errno == ERANGE) return fail(error, "json.number_range");
    *value = JsonValue(static_cast<std::int64_t>(parsed));
    return true;
  }

  bool enter(std::string* error) {
    ++depth_;
    if (depth_ > 32) return fail(error, "json.depth");
    return true;
  }

  bool parse_array(JsonValue* value, std::string* error) {
    if (!enter(error)) return false;
    consume('[');
    *value = JsonValue::array();
    skip_space();
    if (consume(']')) { --depth_; return true; }
    for (;;) {
      JsonValue item;
      if (!parse_value(&item, error)) return false;
      value->array_items().push_back(item);
      skip_space();
      if (consume(']')) { --depth_; return true; }
      if (!consume(',')) return fail(error, "json.expected_comma");
      skip_space();
    }
  }

  bool parse_object(JsonValue* value, std::string* error) {
    if (!enter(error)) return false;
    consume('{');
    *value = JsonValue::object();
    skip_space();
    if (consume('}')) { --depth_; return true; }
    for (;;) {
      std::string name;
      if (!parse_string(&name, error)) return false;
      skip_space();
      if (!consume(':')) return fail(error, "json.expected_colon");
      skip_space();
      JsonValue item;
      if (!parse_value(&item, error)) return false;
      if (!value->object_items().insert(std::make_pair(name, item)).second) {
        return fail(error, "json.duplicate_property");
      }
      skip_space();
      if (consume('}')) { --depth_; return true; }
      if (!consume(',')) return fail(error, "json.expected_comma");
      skip_space();
    }
  }
};

void write_escaped(const std::string& value, std::ostringstream* output) {
  *output << '"';
  const char* digits = "0123456789abcdef";
  for (unsigned char character : value) {
    switch (character) {
      case '"': *output << "\\\""; break;
      case '\\': *output << "\\\\"; break;
      case '\b': *output << "\\b"; break;
      case '\f': *output << "\\f"; break;
      case '\n': *output << "\\n"; break;
      case '\r': *output << "\\r"; break;
      case '\t': *output << "\\t"; break;
      default:
        if (character < 0x20) {
          *output << "\\u00" << digits[(character >> 4) & 0xF] << digits[character & 0xF];
        } else {
          *output << static_cast<char>(character);
        }
    }
  }
  *output << '"';
}

void write_value(const JsonValue& value, std::ostringstream* output) {
  switch (value.type()) {
    case JsonValue::Type::null_value: *output << "null"; break;
    case JsonValue::Type::boolean: *output << (value.boolean_value() ? "true" : "false"); break;
    case JsonValue::Type::number: *output << value.number_value(); break;
    case JsonValue::Type::string: write_escaped(value.string_value(), output); break;
    case JsonValue::Type::array: {
      *output << '[';
      bool first = true;
      for (const JsonValue& item : value.array_items()) {
        if (!first) *output << ',';
        first = false;
        write_value(item, output);
      }
      *output << ']';
      break;
    }
    case JsonValue::Type::object: {
      *output << '{';
      bool first = true;
      for (const std::pair<const std::string, JsonValue>& item : value.object_items()) {
        if (!first) *output << ',';
        first = false;
        write_escaped(item.first, output);
        *output << ':';
        write_value(item.second, output);
      }
      *output << '}';
      break;
    }
  }
}

}  // namespace

bool parse_json(const std::string& input, JsonValue* value, std::string* error) {
  if (value == nullptr) return false;
  return Parser(input).parse(value, error);
}

std::string write_json(const JsonValue& value) {
  std::ostringstream output;
  write_value(value, &output);
  return output.str();
}

}  // namespace usb_dlp
