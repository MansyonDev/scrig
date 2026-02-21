#include "scrig/platform.hpp"

namespace scrig {

void apply_platform_default_config(Config& config) {
  config.randomx_huge_pages = false;
  config.randomx_secure = true;
  config.randomx_macos_unsafe = true;
  config.numa_bind = false;
  config.performance_cores_only = false;
}

const char* platform_profile_name() {
  return "macos-performance";
}

const char* platform_config_comment() {
  return "macOS Specific Config";
}

PlatformConfigLayout platform_config_layout() {
  PlatformConfigLayout layout;
  layout.include_pin_threads = true;
  layout.include_numa_bind = false;
  layout.include_randomx_huge_pages = false;
  layout.include_randomx_macos_unsafe = true;
  return layout;
}

void apply_platform_runtime_safety(Config& config, std::vector<std::string>& notes) {
  if (config.randomx_jit && !config.randomx_macos_unsafe) {
    notes.push_back("randomx_jit requested on macOS stable profile; set randomx_macos_unsafe=true if you want to force it");
  }

  if (config.randomx_full_mem && !config.randomx_macos_unsafe) {
    notes.push_back(
      "randomx_full_mem requested on macOS stable profile; set randomx_macos_unsafe=true if you want to force it");
  }

  if (config.randomx_jit && !config.randomx_secure) {
    notes.push_back("randomx_jit is enabled while randomx_secure=false; this can cause bus errors on macOS");
  }
}

} // namespace scrig
