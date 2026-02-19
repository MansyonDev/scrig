#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scrig {

struct AffinitySlot {
  uint32_t os_cpu = 0;
  uint32_t package_id = 0;
  uint32_t core_id = 0;
  uint32_t sibling_index = 0;
  uint32_t performance_tier = 0;
};

struct AffinityPlanData {
  std::string source = "fallback";
  uint32_t logical_cpus = 0;
  uint32_t physical_cores = 0;
  std::vector<AffinitySlot> slots;
};

bool os_build_affinity_plan(AffinityPlanData* plan);
bool os_pin_current_thread(const AffinitySlot& slot);
bool os_bind_current_thread_numa(uint32_t worker_index, uint32_t worker_count);
bool os_apply_mining_thread_priority(uint32_t worker_index, const AffinitySlot* slot, bool performance_cores_only);
bool os_thread_pinning_supported();
bool os_numa_binding_supported();
bool os_huge_pages_supported_on_platform();
bool os_can_detect_huge_pages_configuration();
bool os_huge_pages_configured();
bool os_numa_detected();
std::string os_platform_tuning_summary(const std::string& affinity_source);

} // namespace scrig
