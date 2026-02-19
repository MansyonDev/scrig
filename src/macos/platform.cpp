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
    config.randomx_jit = false;
    notes.push_back("randomx_jit disabled on macOS stability profile (set randomx_macos_unsafe=true to force jit)");
  }

  if (config.randomx_full_mem && !config.randomx_macos_unsafe) {
    config.randomx_full_mem = false;
    notes.push_back(
      "randomx_full_mem disabled on macOS stability profile (set randomx_macos_unsafe=true to force full_mem)");
  }

  if (config.randomx_jit && !config.randomx_secure) {
    config.randomx_secure = true;
    notes.push_back("randomx_secure enabled on macOS when JIT is active to avoid bus error crashes");
  }
}

} // namespace scrig
