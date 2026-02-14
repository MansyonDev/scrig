#pragma once

#include <cstdint>
#include <string>

namespace scrig {

uint32_t logical_cpu_count();
bool pin_current_thread(uint32_t worker_index, uint32_t worker_count);
bool bind_current_thread_numa(uint32_t worker_index, uint32_t worker_count);
bool thread_pinning_supported();
bool numa_binding_supported();
bool huge_pages_supported_on_platform();
bool can_detect_huge_pages_configuration();
bool huge_pages_configured();
bool numa_detected();
std::string platform_tuning_summary();

} // namespace scrig
