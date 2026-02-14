#include "scrig/json.hpp"

#include <charconv>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>

namespace scrig {

JsonValue::JsonValue() : value_(nullptr) {}
JsonValue::JsonValue(std::nullptr_t value) : value_(value) {}
JsonValue::JsonValue(bool value) : value_(value) {}
JsonValue::JsonValue(int32_t value) : value_(static_cast<int64_t>(value)) {}
JsonValue::JsonValue(int64_t value) : value_(value) {}
JsonValue::JsonValue(uint32_t value) : value_(static_cast<uint64_t>(value)) {}
JsonValue::JsonValue(uint64_t value) : value_(value) {}
JsonValue::JsonValue(double value) : value_(value) {}
JsonValue::JsonValue(std::string value) : value_(std::move(value)) {}
JsonValue::JsonValue(const char* value) : value_(std::string(value)) {}
JsonValue::JsonValue(array value) : value_(std::move(value)) {}
JsonValue::JsonValue(object value) : value_(std::move(value)) {}

bool JsonValue::is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
bool JsonValue::is_bool() const { return std::holds_alternative<bool>(value_); }
bool JsonValue::is_int64() const { return std::holds_alternative<int64_t>(value_); }
bool JsonValue::is_uint64() const { return std::holds_alternative<uint64_t>(value_); }
bool JsonValue::is_double() const { return std::holds_alternative<double>(value_); }
bool JsonValue::is_number() const { return is_int64() || is_uint64() || is_double(); }
bool JsonValue::is_string() const { return std::holds_alternative<std::string>(value_); }
bool JsonValue::is_array() const { return std::holds_alternative<array>(value_); }
bool JsonValue::is_object() const { return std::holds_alternative<object>(value_); }

bool JsonValue::as_bool() const {
  if (!is_bool()) {
    throw JsonError("expected bool");
  }
  return std::get<bool>(value_);
}

int64_t JsonValue::as_int64() const {
  if (is_int64()) {
    return std::get<int64_t>(value_);
  }
  if (is_uint64()) {
    const auto value = std::get<uint64_t>(value_);
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throw JsonError("number out of range for int64");
    }
    return static_cast<int64_t>(value);
  }
  if (is_double()) {
    return static_cast<int64_t>(std::get<double>(value_));
  }
  throw JsonError("expected int64");
}

uint64_t JsonValue::as_uint64() const {
  if (is_uint64()) {
    return std::get<uint64_t>(value_);
  }
  if (is_int64()) {
    const auto value = std::get<int64_t>(value_);
    if (value < 0) {
      throw JsonError("negative number for uint64");
    }
    return static_cast<uint64_t>(value);
  }
  if (is_double()) {
    const auto value = std::get<double>(value_);
    if (value < 0.0) {
      throw JsonError("negative number for uint64");
    }
    return static_cast<uint64_t>(value);
  }
  throw JsonError("expected uint64");
}

double JsonValue::as_double() const {
  if (is_double()) {
    return std::get<double>(value_);
  }
  if (is_int64()) {
    return static_cast<double>(std::get<int64_t>(value_));
  }
  if (is_uint64()) {
    return static_cast<double>(std::get<uint64_t>(value_));
  }
  throw JsonError("expected double");
}

const std::string& JsonValue::as_string() const {
  if (!is_string()) {
    throw JsonError("expected string");
  }
  return std::get<std::string>(value_);
}

const JsonValue::array& JsonValue::as_array() const {
  if (!is_array()) {
    throw JsonError("expected array");
  }
  return std::get<array>(value_);
}

const JsonValue::object& JsonValue::as_object() const {
  if (!is_object()) {
    throw JsonError("expected object");
  }
  return std::get<object>(value_);
}

JsonValue::array& JsonValue::as_array() {
  if (!is_array()) {
    throw JsonError("expected array");
  }
  return std::get<array>(value_);
}

JsonValue::object& JsonValue::as_object() {
  if (!is_object()) {
    throw JsonError("expected object");
  }
  return std::get<object>(value_);
}

const JsonValue* JsonValue::find(const std::string& key) const {
  if (!is_object()) {
    return nullptr;
  }
  const auto& obj = std::get<object>(value_);
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return nullptr;
  }
  return &it->second;
}

JsonValue* JsonValue::find(const std::string& key) {
  if (!is_object()) {
    return nullptr;
  }
  auto& obj = std::get<object>(value_);
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return nullptr;
  }
  return &it->second;
}

const JsonValue::storage& JsonValue::raw() const { return value_; }
JsonValue::storage& JsonValue::raw() { return value_; }

namespace {

class Parser {
public:
  explicit Parser(const std::string& text) : text_(text) {}

  JsonValue parse() {
    skip_ws();
    auto value = parse_value();
    skip_ws();
    if (!eof()) {
      throw error("unexpected trailing data");
    }
    return value;
  }

private:
  const std::string& text_;
  size_t pos_ = 0;

  bool eof() const { return pos_ >= text_.size(); }

  char peek() const {
    if (eof()) {
      return '\0';
    }
    return text_[pos_];
  }

  char next() {
    if (eof()) {
      throw error("unexpected end of input");
    }
    return text_[pos_++];
  }

  [[nodiscard]] JsonError error(const std::string& msg) const {
    return JsonError(msg + " at position " + std::to_string(pos_));
  }

  void skip_ws() {
    while (!eof() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
      ++pos_;
    }
  }

  JsonValue parse_value() {
    if (eof()) {
      throw error("unexpected end of input");
    }
    switch (peek()) {
      case 'n':
        parse_literal("null");
        return JsonValue(nullptr);
      case 't':
        parse_literal("true");
        return JsonValue(true);
      case 'f':
        parse_literal("false");
        return JsonValue(false);
      case '"':
        return JsonValue(parse_string());
      case '[':
        return JsonValue(parse_array());
      case '{':
        return JsonValue(parse_object());
      default:
        if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek())) != 0) {
          return parse_number();
        }
        throw error("unexpected token");
    }
  }

  void parse_literal(const char* lit) {
    for (size_t i = 0; lit[i] != '\0'; ++i) {
      if (next() != lit[i]) {
        throw error("invalid literal");
      }
    }
  }

  std::string parse_string() {
    if (next() != '"') {
      throw error("expected string");
    }

    std::string out;
    out.reserve(32);

    while (!eof()) {
      const char c = next();
      if (c == '"') {
        return out;
      }
      if (c == '\\') {
        const char esc = next();
        switch (esc) {
          case '"':
          case '\\':
          case '/':
            out.push_back(esc);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            // Keep unicode escapes escaped; this is sufficient for API fields used by miner.
            out.append("\\u");
            for (int i = 0; i < 4; ++i) {
              const char h = next();
              if (!std::isxdigit(static_cast<unsigned char>(h))) {
                throw error("invalid unicode escape");
              }
              out.push_back(h);
            }
            break;
          }
          default:
            throw error("invalid string escape");
        }
      } else {
        out.push_back(c);
      }
    }

    throw error("unterminated string");
  }

  JsonValue parse_number() {
    const size_t start = pos_;

    if (peek() == '-') {
      ++pos_;
    }

    if (peek() == '0') {
      ++pos_;
    } else {
      if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
        throw error("invalid number");
      }
      while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        ++pos_;
      }
    }

    bool is_float = false;

    if (!eof() && peek() == '.') {
      is_float = true;
      ++pos_;
      if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
        throw error("invalid number");
      }
      while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        ++pos_;
      }
    }

    if (!eof() && (peek() == 'e' || peek() == 'E')) {
      is_float = true;
      ++pos_;
      if (peek() == '+' || peek() == '-') {
        ++pos_;
      }
      if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
        throw error("invalid number");
      }
      while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        ++pos_;
      }
    }

    const std::string_view token(text_.data() + start, pos_ - start);

    if (is_float) {
      double out = 0.0;
      const auto res = std::from_chars(token.data(), token.data() + token.size(), out);
      if (res.ec != std::errc()) {
        throw error("invalid floating-point number");
      }
      return JsonValue(out);
    }

    if (!token.empty() && token.front() == '-') {
      int64_t out = 0;
      const auto res = std::from_chars(token.data(), token.data() + token.size(), out);
      if (res.ec != std::errc()) {
        throw error("invalid signed number");
      }
      return JsonValue(out);
    }

    uint64_t out = 0;
    const auto res = std::from_chars(token.data(), token.data() + token.size(), out);
    if (res.ec != std::errc()) {
      throw error("invalid unsigned number");
    }
    return JsonValue(out);
  }

  JsonValue::array parse_array() {
    if (next() != '[') {
      throw error("expected array");
    }

    JsonValue::array out;
    skip_ws();
    if (peek() == ']') {
      ++pos_;
      return out;
    }

    while (true) {
      skip_ws();
      out.push_back(parse_value());
      skip_ws();
      const char c = next();
      if (c == ']') {
        break;
      }
      if (c != ',') {
        throw error("expected ',' or ']' in array");
      }
    }

    return out;
  }

  JsonValue::object parse_object() {
    if (next() != '{') {
      throw error("expected object");
    }

    JsonValue::object out;
    skip_ws();
    if (peek() == '}') {
      ++pos_;
      return out;
    }

    while (true) {
      skip_ws();
      if (peek() != '"') {
        throw error("expected object key");
      }
      std::string key = parse_string();
      skip_ws();
      if (next() != ':') {
        throw error("expected ':' after object key");
      }
      skip_ws();
      out.emplace(std::move(key), parse_value());
      skip_ws();
      const char c = next();
      if (c == '}') {
        break;
      }
      if (c != ',') {
        throw error("expected ',' or '}' in object");
      }
    }

    return out;
  }
};

void append_indent(std::string& out, uint32_t level, uint32_t width) {
  out.append(static_cast<size_t>(level) * static_cast<size_t>(width), ' ');
}

void escape_string(const std::string& in, std::string& out) {
  out.push_back('"');
  for (const char c : in) {
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20U) {
          std::ostringstream oss;
          oss << "\\u" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(c));
          out.append(oss.str());
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
}

void stringify_impl(const JsonValue& value, std::string& out, bool pretty, uint32_t indent, uint32_t level) {
  if (value.is_null()) {
    out.append("null");
    return;
  }
  if (value.is_bool()) {
    out.append(value.as_bool() ? "true" : "false");
    return;
  }
  if (value.is_int64()) {
    out.append(std::to_string(value.as_int64()));
    return;
  }
  if (value.is_uint64()) {
    out.append(std::to_string(value.as_uint64()));
    return;
  }
  if (value.is_double()) {
    std::ostringstream oss;
    oss << std::setprecision(16) << value.as_double();
    out.append(oss.str());
    return;
  }
  if (value.is_string()) {
    escape_string(value.as_string(), out);
    return;
  }
  if (value.is_array()) {
    const auto& arr = value.as_array();
    out.push_back('[');
    if (!arr.empty()) {
      for (size_t i = 0; i < arr.size(); ++i) {
        if (pretty) {
          out.push_back('\n');
          append_indent(out, level + 1, indent);
        }
        stringify_impl(arr[i], out, pretty, indent, level + 1);
        if (i + 1 != arr.size()) {
          out.push_back(',');
        }
      }
      if (pretty) {
        out.push_back('\n');
        append_indent(out, level, indent);
      }
    }
    out.push_back(']');
    return;
  }

  const auto& obj = value.as_object();
  out.push_back('{');
  if (!obj.empty()) {
    size_t i = 0;
    for (const auto& [key, child] : obj) {
      if (pretty) {
        out.push_back('\n');
        append_indent(out, level + 1, indent);
      }
      escape_string(key, out);
      out.push_back(':');
      if (pretty) {
        out.push_back(' ');
      }
      stringify_impl(child, out, pretty, indent, level + 1);
      if (i + 1 != obj.size()) {
        out.push_back(',');
      }
      ++i;
    }
    if (pretty) {
      out.push_back('\n');
      append_indent(out, level, indent);
    }
  }
  out.push_back('}');
}

} // namespace

JsonValue parse_json(const std::string& text) {
  Parser parser(text);
  return parser.parse();
}

std::string to_json(const JsonValue& value, bool pretty, uint32_t indent) {
  std::string out;
  stringify_impl(value, out, pretty, indent, 0);
  return out;
}

} // namespace scrig

