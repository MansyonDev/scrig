#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace scrig {

uint32_t logical_cpu_count();
uint32_t physical_cpu_count();
uint32_t recommended_mining_threads(bool performance_cores_only = false);
bool hybrid_topology_detected();
uint32_t performance_core_logical_count();
uint32_t performance_core_physical_count();
std::string cpu_runtime_summary();
std::string affinity_plan_source();
std::string affinity_profile_summary(uint32_t worker_count, size_t max_workers = 8);
bool pin_current_thread(uint32_t worker_index, uint32_t worker_count, bool performance_cores_only = false);
bool bind_current_thread_numa(uint32_t worker_index, uint32_t worker_count);
bool apply_mining_thread_priority(uint32_t worker_index, uint32_t worker_count, bool performance_cores_only = false);
bool thread_pinning_supported();
bool numa_binding_supported();
bool huge_pages_supported_on_platform();
bool can_detect_huge_pages_configuration();
bool huge_pages_configured();
bool numa_detected();
std::string platform_tuning_summary();

} // namespace scrig
