#include "scrig/platform.hpp"

namespace scrig {

void apply_platform_default_config(Config&) {
}

const char* platform_profile_name() {
  return "generic";
}

const char* platform_config_comment() {
  return "Generic OS Config";
}

PlatformConfigLayout platform_config_layout() {
  return PlatformConfigLayout{};
}

void apply_platform_runtime_safety(Config&, std::vector<std::string>&) {
}

} // namespace scrig
