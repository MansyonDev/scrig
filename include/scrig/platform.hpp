#pragma once

#include "scrig/config.hpp"

#include <string>
#include <vector>

namespace scrig {

struct PlatformConfigLayout {
  bool include_pin_threads = true;
  bool include_numa_bind = true;
  bool include_randomx_huge_pages = true;
  bool include_randomx_macos_unsafe = false;
};

void apply_platform_default_config(Config& config);
const char* platform_profile_name();
const char* platform_config_comment();
PlatformConfigLayout platform_config_layout();
void apply_platform_runtime_safety(Config& config, std::vector<std::string>& notes);

} // namespace scrig
