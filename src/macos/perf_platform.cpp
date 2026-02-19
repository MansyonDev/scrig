#include "scrig/perf_platform.hpp"

#include <cstdio>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/sysctl.h>

namespace scrig {

bool os_build_affinity_plan(AffinityPlanData* plan) {
  int nperf = 0;
  size_t nperf_size = sizeof(nperf);
  if (sysctlbyname("hw.nperflevels", &nperf, &nperf_size, nullptr, 0) == 0 && nperf > 0) {
    uint32_t logical_total = 0;
    uint32_t physical_total = 0;
    uint32_t core_base = 0;

    for (int level = 0; level < nperf; ++level) {
      char logical_key[64];
      char physical_key[64];
      std::snprintf(logical_key, sizeof(logical_key), "hw.perflevel%d.logicalcpu", level);
      std::snprintf(physical_key, sizeof(physical_key), "hw.perflevel%d.physicalcpu", level);

      int level_logical = 0;
      int level_physical = 0;
      size_t size = sizeof(int);
      if (sysctlbyname(logical_key, &level_logical, &size, nullptr, 0) != 0 || level_logical <= 0) {
        continue;
      }
      size = sizeof(int);
      if (sysctlbyname(physical_key, &level_physical, &size, nullptr, 0) != 0 || level_physical <= 0) {
        level_physical = level_logical;
      }
      if (level_physical > level_logical) {
        level_physical = level_logical;
      }

      for (int i = 0; i < level_logical; ++i) {
        AffinitySlot slot;
        slot.os_cpu = logical_total + static_cast<uint32_t>(i);
        slot.package_id = 0;
        slot.core_id = core_base + static_cast<uint32_t>(i % level_physical);
        slot.sibling_index = static_cast<uint32_t>(i / level_physical);
        slot.performance_tier = static_cast<uint32_t>(level);
        plan->slots.push_back(slot);
      }

      logical_total += static_cast<uint32_t>(level_logical);
      physical_total += static_cast<uint32_t>(level_physical);
      core_base += static_cast<uint32_t>(level_physical);
    }

    if (!plan->slots.empty()) {
      plan->source = "macos-perflevels";
      plan->logical_cpus = logical_total;
      plan->physical_cores = physical_total == 0 ? logical_total : physical_total;
      return true;
    }
  }

  int logical = 0;
  int physical = 0;
  size_t size = sizeof(int);
  if (sysctlbyname("hw.logicalcpu", &logical, &size, nullptr, 0) != 0 || logical <= 0) {
    return false;
  }
  size = sizeof(int);
  if (sysctlbyname("hw.physicalcpu", &physical, &size, nullptr, 0) != 0 || physical <= 0) {
    physical = logical;
  }

  for (int i = 0; i < logical; ++i) {
    AffinitySlot slot;
    slot.os_cpu = static_cast<uint32_t>(i);
    slot.package_id = 0;
    slot.core_id = static_cast<uint32_t>(i % physical);
    slot.sibling_index = static_cast<uint32_t>(i / physical);
    slot.performance_tier = 0;
    plan->slots.push_back(slot);
  }

  plan->source = "macos-sysctl";
  plan->logical_cpus = static_cast<uint32_t>(logical);
  plan->physical_cores = static_cast<uint32_t>(physical);
  return true;
}

bool os_pin_current_thread(const AffinitySlot& slot) {
  thread_affinity_policy_data_t policy{};
  policy.affinity_tag = static_cast<integer_t>((slot.core_id % 0x7FFFFFFFU) + 1U);
  const thread_t thread = mach_thread_self();
  const kern_return_t result = thread_policy_set(
    thread,
    THREAD_AFFINITY_POLICY,
    reinterpret_cast<thread_policy_t>(&policy),
    THREAD_AFFINITY_POLICY_COUNT);
  (void)mach_port_deallocate(mach_task_self(), thread);
  return result == KERN_SUCCESS;
}

bool os_bind_current_thread_numa(uint32_t, uint32_t) {
  return false;
}

bool os_apply_mining_thread_priority(uint32_t, const AffinitySlot*, bool performance_cores_only) {
  const qos_class_t qos = performance_cores_only ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED;
  return pthread_set_qos_class_self_np(qos, 0) == 0;
}

bool os_thread_pinning_supported() {
  return true;
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

std::string os_platform_tuning_summary(const std::string& affinity_source) {
  return "macos huge_pages=n/a numa=n/a affinity=" + affinity_source;
}

} // namespace scrig
