#include "scrig/perf_platform.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <string>

#include <processthreadsapi.h>
#include <winbase.h>
#include <windows.h>

namespace scrig {

namespace {

bool windows_large_page_privilege_enabled() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }

  LUID luid{};
  if (!LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &luid)) {
    CloseHandle(token);
    return false;
  }

  PRIVILEGE_SET required{};
  required.PrivilegeCount = 1;
  required.Control = PRIVILEGE_SET_ALL_NECESSARY;
  required.Privilege[0].Luid = luid;
  required.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;

  BOOL has_privilege = FALSE;
  const BOOL ok = PrivilegeCheck(token, &required, &has_privilege);
  CloseHandle(token);
  return ok == TRUE && has_privilege == TRUE;
}

} // namespace

bool os_build_affinity_plan(AffinityPlanData* plan) {
  DWORD len = 0;
  (void)GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  if (len == 0) {
    return false;
  }

  std::vector<uint8_t> buffer(len);
  if (!GetLogicalProcessorInformationEx(
        RelationProcessorCore,
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
        &len)) {
    return false;
  }

  uint32_t core_counter = 0;
  size_t offset = 0;
  while (offset < buffer.size()) {
    auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
    if (info->Relationship == RelationProcessorCore) {
      const auto& proc = info->Processor;
      for (WORD g = 0; g < proc.GroupCount; ++g) {
        const GROUP_AFFINITY& group = proc.GroupMask[g];
        uint32_t sibling = 0;
        for (uint32_t bit = 0; bit < 64U; ++bit) {
          const KAFFINITY mask = static_cast<KAFFINITY>(1ULL << bit);
          if ((group.Mask & mask) == 0) {
            continue;
          }

          AffinitySlot slot;
          slot.os_cpu = static_cast<uint32_t>(group.Group) * 64U + bit;
          slot.package_id = 0;
          slot.core_id = core_counter;
          slot.sibling_index = sibling++;
          slot.performance_tier = static_cast<uint32_t>(proc.EfficiencyClass);
          plan->slots.push_back(slot);
        }
      }
      ++core_counter;
    }

    if (info->Size == 0) {
      break;
    }
    offset += info->Size;
  }

  plan->source = "windows-topology";
  plan->logical_cpus = static_cast<uint32_t>(plan->slots.size());
  plan->physical_cores = core_counter;
  return !plan->slots.empty();
}

bool os_pin_current_thread(const AffinitySlot& slot) {
  const WORD group = static_cast<WORD>(slot.os_cpu / 64U);
  const KAFFINITY mask = static_cast<KAFFINITY>(1ULL << (slot.os_cpu % 64U));

  GROUP_AFFINITY affinity{};
  affinity.Group = group;
  affinity.Mask = mask;
  if (SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != 0) {
    return true;
  }

  if (group == 0 && mask != 0) {
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask)) != 0;
  }
  return false;
}

bool os_bind_current_thread_numa(uint32_t, uint32_t) {
  return false;
}

bool os_apply_mining_thread_priority(uint32_t, const AffinitySlot* slot, bool) {
  if (slot != nullptr) {
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0601
    PROCESSOR_NUMBER proc{};
    proc.Group = static_cast<WORD>(slot->os_cpu / 64U);
    proc.Number = static_cast<BYTE>(slot->os_cpu % 64U);
    proc.Reserved = 0;
    (void)SetThreadIdealProcessorEx(GetCurrentThread(), &proc, nullptr);
#endif
  }

  const BOOL priority_ok = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  (void)SetThreadPriorityBoost(GetCurrentThread(), TRUE);
  return priority_ok != 0;
}

bool os_thread_pinning_supported() {
  return true;
}

bool os_numa_binding_supported() {
  return false;
}

bool os_huge_pages_supported_on_platform() {
  return true;
}

bool os_can_detect_huge_pages_configuration() {
  return false;
}

bool os_huge_pages_configured() {
  return GetLargePageMinimum() > 0 && windows_large_page_privilege_enabled();
}

bool os_numa_detected() {
  return false;
}

std::string os_platform_tuning_summary(const std::string& affinity_source) {
  return "windows huge_pages=manual numa=auto affinity=" + affinity_source;
}

} // namespace scrig
