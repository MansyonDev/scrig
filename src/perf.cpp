#include "scrig/perf.hpp"

#include "scrig/perf_platform.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef SCRIG_HAVE_HWLOC
#include <hwloc.h>
#endif

namespace scrig {

namespace {

std::once_flag g_affinity_plan_once;
AffinityPlanData g_affinity_plan;

uint32_t logical_cpu_count_fallback() {
  const auto hw = std::thread::hardware_concurrency();
  return hw == 0 ? 1U : hw;
}

void sort_affinity_slots(std::vector<AffinitySlot>* slots) {
  std::sort(slots->begin(), slots->end(), [](const AffinitySlot& a, const AffinitySlot& b) {
    if (a.performance_tier != b.performance_tier) {
      return a.performance_tier < b.performance_tier;
    }
    if (a.sibling_index != b.sibling_index) {
      return a.sibling_index < b.sibling_index;
    }
    if (a.package_id != b.package_id) {
      return a.package_id < b.package_id;
    }
    if (a.core_id != b.core_id) {
      return a.core_id < b.core_id;
    }
    return a.os_cpu < b.os_cpu;
  });
}

void finalize_affinity_plan(AffinityPlanData* plan) {
  if (plan->slots.empty()) {
    const uint32_t logical = logical_cpu_count_fallback();
    plan->source = "fallback";
    plan->logical_cpus = logical;
    plan->physical_cores = logical;
    plan->slots.reserve(logical);
    for (uint32_t i = 0; i < logical; ++i) {
      AffinitySlot slot;
      slot.os_cpu = i;
      slot.package_id = 0;
      slot.core_id = i;
      slot.sibling_index = 0;
      slot.performance_tier = 0;
      plan->slots.push_back(slot);
    }
  }

  sort_affinity_slots(&plan->slots);

  if (plan->logical_cpus == 0) {
    plan->logical_cpus = static_cast<uint32_t>(plan->slots.size());
  }

  if (plan->physical_cores == 0) {
    std::set<std::pair<uint32_t, uint32_t>> cores;
    for (const auto& slot : plan->slots) {
      cores.emplace(slot.package_id, slot.core_id);
    }
    plan->physical_cores = static_cast<uint32_t>(cores.size());
  }
}

#ifdef SCRIG_HAVE_HWLOC
uint32_t classify_hwloc_performance_tier(hwloc_obj_t obj) {
  if (obj == nullptr) {
    return 0;
  }

  for (unsigned i = 0; i < obj->infos_count; ++i) {
    const char* name = obj->infos[i].name;
    const char* value = obj->infos[i].value;
    if (name == nullptr || value == nullptr) {
      continue;
    }

    std::string key(name);
    std::string val(value);
    for (char& ch : key) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    for (char& ch : val) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    if (key.find("coretype") != std::string::npos || key.find("uarch") != std::string::npos) {
      if (val.find("atom") != std::string::npos ||
          val.find("eff") != std::string::npos ||
          val.find("little") != std::string::npos) {
        return 1;
      }
      if (val.find("core") != std::string::npos ||
          val.find("perf") != std::string::npos ||
          val.find("big") != std::string::npos) {
        return 0;
      }
      try {
        return static_cast<uint32_t>(std::stoul(val));
      } catch (...) {
      }
    }
  }

  return 0;
}

bool build_affinity_plan_hwloc(AffinityPlanData* plan) {
  hwloc_topology_t topology = nullptr;
  if (hwloc_topology_init(&topology) != 0 || topology == nullptr) {
    return false;
  }

  if (hwloc_topology_load(topology) != 0) {
    hwloc_topology_destroy(topology);
    return false;
  }

  const int pu_depth = hwloc_get_type_depth(topology, HWLOC_OBJ_PU);
  if (pu_depth < 0) {
    hwloc_topology_destroy(topology);
    return false;
  }

  const int pu_count = hwloc_get_nbobjs_by_depth(topology, pu_depth);
  if (pu_count <= 0) {
    hwloc_topology_destroy(topology);
    return false;
  }

  std::map<std::pair<uint32_t, uint32_t>, uint32_t> sibling_counts;

  for (int i = 0; i < pu_count; ++i) {
    hwloc_obj_t pu = hwloc_get_obj_by_depth(topology, pu_depth, i);
    if (pu == nullptr) {
      continue;
    }

    AffinitySlot slot;
    slot.os_cpu = pu->os_index == static_cast<unsigned>(-1)
      ? static_cast<uint32_t>(i)
      : static_cast<uint32_t>(pu->os_index);

    hwloc_obj_t core = hwloc_get_ancestor_obj_by_type(topology, HWLOC_OBJ_CORE, pu);
    hwloc_obj_t package = hwloc_get_ancestor_obj_by_type(topology, HWLOC_OBJ_PACKAGE, pu);

    slot.core_id = core == nullptr ? slot.os_cpu : static_cast<uint32_t>(core->logical_index);
    slot.package_id = package == nullptr ? 0U : static_cast<uint32_t>(package->logical_index);
    slot.performance_tier = classify_hwloc_performance_tier(core != nullptr ? core : pu);

    auto key = std::make_pair(slot.package_id, slot.core_id);
    auto it = sibling_counts.find(key);
    if (it == sibling_counts.end()) {
      slot.sibling_index = 0;
      sibling_counts.emplace(key, 1U);
    } else {
      slot.sibling_index = it->second;
      ++it->second;
    }

    plan->slots.push_back(slot);
  }

  plan->source = "hwloc";
  plan->logical_cpus = static_cast<uint32_t>(plan->slots.size());
  plan->physical_cores = static_cast<uint32_t>(sibling_counts.size());

  hwloc_topology_destroy(topology);
  return !plan->slots.empty();
}
#endif

const AffinityPlanData& affinity_plan() {
  std::call_once(g_affinity_plan_once, []() {
    AffinityPlanData plan;

    bool built = false;
#ifdef SCRIG_HAVE_HWLOC
    built = build_affinity_plan_hwloc(&plan);
#endif
    if (!built) {
      built = os_build_affinity_plan(&plan);
    }
    (void)built;

    finalize_affinity_plan(&plan);
    g_affinity_plan = std::move(plan);
  });

  return g_affinity_plan;
}

bool hybrid_topology_detected_internal() {
  const auto& plan = affinity_plan();
  if (plan.slots.empty()) {
    return false;
  }

  const uint32_t first_tier = plan.slots.front().performance_tier;
  for (const auto& slot : plan.slots) {
    if (slot.performance_tier != first_tier) {
      return true;
    }
  }
  return false;
}

uint32_t performance_prefix_count() {
  const auto& plan = affinity_plan();
  if (plan.slots.empty()) {
    return 0;
  }

  const uint32_t first_tier = plan.slots.front().performance_tier;
  uint32_t count = 0;
  for (const auto& slot : plan.slots) {
    if (slot.performance_tier != first_tier) {
      break;
    }
    ++count;
  }
  return count;
}

const AffinitySlot* affinity_slot_for_worker(uint32_t worker_index, bool performance_cores_only) {
  const auto& plan = affinity_plan();
  if (plan.slots.empty()) {
    return nullptr;
  }

  if (performance_cores_only && hybrid_topology_detected_internal()) {
    const uint32_t perf_count = performance_prefix_count();
    if (perf_count > 0) {
      return &plan.slots[worker_index % perf_count];
    }
  }

  return &plan.slots[worker_index % static_cast<uint32_t>(plan.slots.size())];
}

} // namespace

uint32_t logical_cpu_count() {
  return affinity_plan().logical_cpus == 0 ? logical_cpu_count_fallback() : affinity_plan().logical_cpus;
}

uint32_t physical_cpu_count() {
  const auto& plan = affinity_plan();
  return plan.physical_cores == 0 ? logical_cpu_count_fallback() : plan.physical_cores;
}

uint32_t recommended_mining_threads(bool performance_cores_only) {
  const uint32_t logical = performance_cores_only ? performance_core_logical_count() : logical_cpu_count();
  const uint32_t physical = performance_cores_only ? performance_core_physical_count() : physical_cpu_count();
  if (physical == 0 || physical > logical) {
    return logical;
  }
  return std::max<uint32_t>(1U, physical);
}

bool hybrid_topology_detected() {
  return hybrid_topology_detected_internal();
}

uint32_t performance_core_logical_count() {
  const auto& plan = affinity_plan();
  if (plan.slots.empty()) {
    return logical_cpu_count_fallback();
  }

  if (!hybrid_topology_detected_internal()) {
    return static_cast<uint32_t>(plan.slots.size());
  }

  return performance_prefix_count();
}

uint32_t performance_core_physical_count() {
  const auto& plan = affinity_plan();
  if (plan.slots.empty()) {
    return logical_cpu_count_fallback();
  }

  const uint32_t perf_count = performance_core_logical_count();
  if (perf_count == 0) {
    return 0;
  }

  std::set<std::pair<uint32_t, uint32_t>> cores;
  for (uint32_t i = 0; i < perf_count; ++i) {
    const auto& slot = plan.slots[i];
    cores.emplace(slot.package_id, slot.core_id);
  }
  return static_cast<uint32_t>(cores.size());
}

std::string cpu_runtime_summary() {
  const auto logical = logical_cpu_count();
  const auto physical = physical_cpu_count();
  const auto recommended = recommended_mining_threads(false);
  const auto recommended_perf = recommended_mining_threads(true);
  const auto source = affinity_plan_source();

  const char* arch = "unknown";
#if defined(__x86_64__) || defined(_M_X64)
  arch = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
  arch = "x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
  arch = "arm64";
#elif defined(__arm__) || defined(_M_ARM)
  arch = "arm";
#endif

  const char* build_mode = "native";
#if defined(SCRIG_PORTABLE_BINARY) && SCRIG_PORTABLE_BINARY
  build_mode = "portable";
#endif

  std::ostringstream out;
  out << "arch=" << arch
      << " build=" << build_mode
      << " physical=" << physical
      << " logical=" << logical
      << " recommended_threads=" << recommended
      << " perf_threads=" << recommended_perf
      << " affinity_source=" << source;
  return out.str();
}

std::string affinity_plan_source() {
  return affinity_plan().source;
}

std::string affinity_profile_summary(uint32_t worker_count, size_t max_workers) {
  const auto& plan = affinity_plan();
  if (plan.slots.empty()) {
    return "affinity unavailable";
  }

  const uint32_t active_workers = worker_count == 0 ? recommended_mining_threads(false) : worker_count;
  const size_t preview = std::min<size_t>(max_workers, static_cast<size_t>(active_workers));

  std::ostringstream out;
  out << "source=" << plan.source
      << " physical=" << plan.physical_cores
      << " logical=" << plan.logical_cpus
      << " mapping=";

  for (size_t i = 0; i < preview; ++i) {
    if (i > 0) {
      out << ' ';
    }
    const auto& slot = plan.slots[i % plan.slots.size()];
    out << 'w' << i << "->cpu" << slot.os_cpu
        << "(tier" << slot.performance_tier;
    if (slot.sibling_index > 0) {
      out << ",s" << slot.sibling_index;
    }
    out << ')';
  }

  if (static_cast<size_t>(active_workers) > preview) {
    out << " ...";
  }

  return out.str();
}

bool pin_current_thread(uint32_t worker_index, uint32_t worker_count, bool performance_cores_only) {
  (void)worker_count;
  const auto* slot = affinity_slot_for_worker(worker_index, performance_cores_only);
  if (slot == nullptr) {
    return false;
  }
  return os_pin_current_thread(*slot);
}

bool bind_current_thread_numa(uint32_t worker_index, uint32_t worker_count) {
  return os_bind_current_thread_numa(worker_index, worker_count);
}

bool apply_mining_thread_priority(uint32_t worker_index, uint32_t worker_count, bool performance_cores_only) {
  (void)worker_count;
  const auto* slot = affinity_slot_for_worker(worker_index, performance_cores_only);
  return os_apply_mining_thread_priority(worker_index, slot, performance_cores_only);
}

bool thread_pinning_supported() {
  return os_thread_pinning_supported();
}

bool numa_binding_supported() {
  return os_numa_binding_supported();
}

bool huge_pages_supported_on_platform() {
  return os_huge_pages_supported_on_platform();
}

bool can_detect_huge_pages_configuration() {
  return os_can_detect_huge_pages_configuration();
}

bool huge_pages_configured() {
  return os_huge_pages_configured();
}

bool numa_detected() {
  return os_numa_detected();
}

std::string platform_tuning_summary() {
  return os_platform_tuning_summary(affinity_plan_source());
}

} // namespace scrig
