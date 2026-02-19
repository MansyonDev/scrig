#include "scrig/perf.hpp"
#include "scrig/platform.hpp"

namespace scrig {

void apply_platform_default_config(Config& config) {
  config.numa_bind = numa_binding_supported() && numa_detected();
  config.performance_cores_only = true;
}

const char* platform_profile_name() {
  return "linux-performance";
}

const char* platform_config_comment() {
  return "Linux Specific Config";
}

PlatformConfigLayout platform_config_layout() {
  PlatformConfigLayout layout;
  layout.include_pin_threads = true;
  layout.include_numa_bind = true;
  layout.include_randomx_huge_pages = true;
  layout.include_randomx_macos_unsafe = false;
  return layout;
}

void apply_platform_runtime_safety(Config&, std::vector<std::string>&) {
}

} // namespace scrig
