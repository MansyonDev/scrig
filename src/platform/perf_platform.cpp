#include "scrig/perf_platform.hpp"

namespace scrig {

bool os_build_affinity_plan(AffinityPlanData*) {
  return false;
}

bool os_pin_current_thread(const AffinitySlot&) {
  return false;
}

bool os_bind_current_thread_numa(uint32_t, uint32_t) {
  return false;
}

bool os_apply_mining_thread_priority(uint32_t, const AffinitySlot*, bool) {
  return false;
}

bool os_thread_pinning_supported() {
  return false;
}

bool os_numa_binding_supported() {
  return false;
}

bool os_huge_pages_supported_on_platform() {
  return false;
}

bool os_can_detect_huge_pages_configuration() {
  return false;
}

bool os_huge_pages_configured() {
  return false;
}

bool os_numa_detected() {
  return false;
}

std::string os_platform_tuning_summary(const std::string&) {
  return "unknown";
}

} // namespace scrig
