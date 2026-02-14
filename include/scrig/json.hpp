#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace scrig {

class JsonError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class JsonValue {
public:
  using array = std::vector<JsonValue>;
  using object = std::map<std::string, JsonValue, std::less<>>;

  using storage = std::variant<std::nullptr_t, bool, int64_t, uint64_t, double, std::string, array, object>;

  JsonValue();
  JsonValue(std::nullptr_t value);
  JsonValue(bool value);
  JsonValue(int32_t value);
  JsonValue(int64_t value);
  JsonValue(uint32_t value);
  JsonValue(uint64_t value);
  JsonValue(double value);
  JsonValue(std::string value);
  JsonValue(const char* value);
  JsonValue(array value);
  JsonValue(object value);

  bool is_null() const;
  bool is_bool() const;
  bool is_int64() const;
  bool is_uint64() const;
  bool is_double() const;
  bool is_number() const;
  bool is_string() const;
  bool is_array() const;
  bool is_object() const;

  bool as_bool() const;
  int64_t as_int64() const;
  uint64_t as_uint64() const;
  double as_double() const;
  const std::string& as_string() const;
  const array& as_array() const;
  const object& as_object() const;
  array& as_array();
  object& as_object();

  const JsonValue* find(const std::string& key) const;
  JsonValue* find(const std::string& key);

  const storage& raw() const;
  storage& raw();

private:
  storage value_;
};

JsonValue parse_json(const std::string& text);
std::string to_json(const JsonValue& value, bool pretty = false, uint32_t indent = 2);

} // namespace scrig

