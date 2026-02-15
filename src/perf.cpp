#include "scrig/perf.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
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

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <fstream>
#ifdef SCRIG_HAVE_LIBNUMA
#include <numa.h>
#endif
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <sys/sysctl.h>
#endif

namespace scrig {

namespace {

struct AffinitySlot {
  uint32_t os_cpu = 0;
  uint32_t package_id = 0;
  uint32_t core_id = 0;
  uint32_t sibling_index = 0;
  uint32_t performance_tier = 0;
#ifdef _WIN32
  WORD group = 0;
  KAFFINITY mask = 0;
#endif
};

struct AffinityPlanData {
  std::string source = "fallback";
  uint32_t logical_cpus = 0;
  uint32_t physical_cores = 0;
  std::vector<AffinitySlot> slots;
};

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
#ifdef _WIN32
      slot.group = static_cast<WORD>(i / 64U);
      slot.mask = static_cast<KAFFINITY>(1ULL << (i % 64U));
#endif
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

#ifdef _WIN32
    slot.group = static_cast<WORD>(slot.os_cpu / 64U);
    slot.mask = static_cast<KAFFINITY>(1ULL << (slot.os_cpu % 64U));
#endif

    plan->slots.push_back(slot);
  }

  plan->source = "hwloc";
  plan->logical_cpus = static_cast<uint32_t>(plan->slots.size());
  plan->physical_cores = static_cast<uint32_t>(sibling_counts.size());

  hwloc_topology_destroy(topology);
  return !plan->slots.empty();
}
#endif

#ifdef _WIN32
bool build_affinity_plan_windows(AffinityPlanData* plan) {
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
          slot.group = group.Group;
          slot.mask = mask;
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
#endif

#if defined(__linux__)
bool parse_linux_cpu_index(const std::string& name, uint32_t* out) {
  if (name.size() < 4 || name.rfind("cpu", 0) != 0) {
    return false;
  }
  const std::string suffix = name.substr(3);
  if (suffix.empty()) {
    return false;
  }
  for (const char ch : suffix) {
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  *out = static_cast<uint32_t>(std::stoul(suffix));
  return true;
}

bool read_linux_topology_int(const std::filesystem::path& path, int* out) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  in >> *out;
  return in.good() || in.eof();
}

bool read_linux_topology_u64(const std::filesystem::path& path, uint64_t* out) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  in >> *out;
  return in.good() || in.eof();
}

bool build_affinity_plan_linux(AffinityPlanData* plan) {
  struct CpuRecord {
    uint32_t cpu = 0;
    uint32_t package = 0;
    uint32_t core = 0;
    int core_type = 0;
    uint64_t max_freq_khz = 0;
  };

  std::vector<CpuRecord> cpus;
  const std::filesystem::path cpu_root{"/sys/devices/system/cpu"};
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(cpu_root, ec)) {
    if (ec || !entry.is_directory()) {
      continue;
    }

    uint32_t cpu_index = 0;
    if (!parse_linux_cpu_index(entry.path().filename().string(), &cpu_index)) {
      continue;
    }

    int core_id = static_cast<int>(cpu_index);
    int package_id = 0;
    int core_type = 0;
    uint64_t max_freq_khz = 0;
    (void)read_linux_topology_int(entry.path() / "topology" / "core_id", &core_id);
    (void)read_linux_topology_int(entry.path() / "topology" / "physical_package_id", &package_id);
    (void)read_linux_topology_int(entry.path() / "topology" / "core_type", &core_type);
    (void)read_linux_topology_u64(entry.path() / "cpufreq" / "cpuinfo_max_freq", &max_freq_khz);

    cpus.push_back(CpuRecord{
      cpu_index,
      static_cast<uint32_t>(std::max(0, package_id)),
      static_cast<uint32_t>(std::max(0, core_id)),
      std::max(0, core_type),
      max_freq_khz,
    });
  }

  if (cpus.empty()) {
    return false;
  }

  std::sort(cpus.begin(), cpus.end(), [](const CpuRecord& a, const CpuRecord& b) {
    return a.cpu < b.cpu;
  });

  std::map<std::pair<uint32_t, uint32_t>, uint32_t> sibling_counts;
  bool has_core_type = false;
  uint64_t max_seen_freq = 0;
  for (const auto& record : cpus) {
    if (record.core_type > 0) {
      has_core_type = true;
    }
    max_seen_freq = std::max<uint64_t>(max_seen_freq, record.max_freq_khz);
  }

  for (const auto& record : cpus) {
    AffinitySlot slot;
    slot.os_cpu = record.cpu;
    slot.package_id = record.package;
    slot.core_id = record.core;
    if (has_core_type) {
      // Intel/Linux core_type: 2=performance core, 1=efficient core.
      if (record.core_type >= 2) {
        slot.performance_tier = 0;
      } else if (record.core_type == 1) {
        slot.performance_tier = 1;
      } else {
        slot.performance_tier = 0;
      }
    } else if (max_seen_freq > 0) {
      // Frequency-based fallback: keep top-frequency cores in tier0.
      const uint64_t threshold = max_seen_freq > 200000 ? (max_seen_freq - 200000) : max_seen_freq;
      slot.performance_tier = record.max_freq_khz >= threshold ? 0 : 1;
    } else {
      slot.performance_tier = 0;
    }

    auto key = std::make_pair(record.package, record.core);
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

  plan->source = "linux-sysfs";
  plan->logical_cpus = static_cast<uint32_t>(plan->slots.size());
  plan->physical_cores = static_cast<uint32_t>(sibling_counts.size());
  return true;
}
#endif

#if defined(__APPLE__)
bool build_affinity_plan_macos(AffinityPlanData* plan) {
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
#endif

const AffinityPlanData& affinity_plan() {
  std::call_once(g_affinity_plan_once, []() {
    AffinityPlanData plan;

#if defined(_WIN32)
    bool built = build_affinity_plan_windows(&plan);
#ifdef SCRIG_HAVE_HWLOC
    if (!built) {
      built = build_affinity_plan_hwloc(&plan);
    }
#endif
    (void)built;
#elif defined(__linux__)
    bool built = false;
#ifdef SCRIG_HAVE_HWLOC
    built = build_affinity_plan_hwloc(&plan);
#endif
    if (!built) {
      built = build_affinity_plan_linux(&plan);
    }
    (void)built;
#elif defined(__APPLE__)
    bool built = false;
#ifdef SCRIG_HAVE_HWLOC
    built = build_affinity_plan_hwloc(&plan);
#endif
    if (!built) {
      built = build_affinity_plan_macos(&plan);
    }
    (void)built;
#endif

    finalize_affinity_plan(&plan);
    g_affinity_plan = std::move(plan);
  });

  return g_affinity_plan;
}

const AffinitySlot* affinity_slot_for_worker(uint32_t worker_index) {
  const auto& plan = affinity_plan();
  if (plan.slots.empty()) {
    return nullptr;
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

uint32_t recommended_mining_threads() {
  const uint32_t logical = logical_cpu_count();
  const uint32_t physical = physical_cpu_count();
  if (physical == 0 || physical > logical) {
    return logical;
  }
  return std::max<uint32_t>(1U, physical);
}

std::string cpu_runtime_summary() {
  const auto logical = logical_cpu_count();
  const auto physical = physical_cpu_count();
  const auto recommended = recommended_mining_threads();
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

  const uint32_t active_workers = worker_count == 0 ? recommended_mining_threads() : worker_count;
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

bool pin_current_thread(uint32_t worker_index, uint32_t worker_count) {
  (void)worker_count;
  const auto* slot = affinity_slot_for_worker(worker_index);
  if (slot == nullptr) {
    return false;
  }

#ifdef _WIN32
  GROUP_AFFINITY affinity{};
  affinity.Group = slot->group;
  affinity.Mask = slot->mask;
  if (SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != 0) {
    return true;
  }

  if (slot->group == 0 && slot->mask != 0) {
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(slot->mask)) != 0;
  }
  return false;
#elif defined(__linux__)
  if (slot->os_cpu >= CPU_SETSIZE) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(slot->os_cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &set) == 0;
#elif defined(__APPLE__)
  thread_affinity_policy_data_t policy{};
  policy.affinity_tag = static_cast<integer_t>((slot->core_id % 0x7FFFFFFFU) + 1U);
  return thread_policy_set(
           mach_thread_self(),
           THREAD_AFFINITY_POLICY,
           reinterpret_cast<thread_policy_t>(&policy),
           THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#else
  return false;
#endif
}

bool bind_current_thread_numa(uint32_t worker_index, uint32_t worker_count) {
  (void)worker_index;
  (void)worker_count;

#if defined(__linux__) && defined(SCRIG_HAVE_LIBNUMA)
  if (numa_available() < 0) {
    return false;
  }

  const int max_node = numa_max_node();
  if (max_node < 1) {
    return false;
  }

  const int nodes = max_node + 1;
  const int node = static_cast<int>(worker_index % static_cast<uint32_t>(nodes));

  return numa_run_on_node(node) == 0;
#else
  return false;
#endif
}

bool thread_pinning_supported() {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
  return true;
#else
  return false;
#endif
}

bool numa_binding_supported() {
#if defined(__linux__) && defined(SCRIG_HAVE_LIBNUMA)
  return numa_available() == 0;
#else
  return false;
#endif
}

bool huge_pages_supported_on_platform() {
#ifdef __linux__
  return true;
#elif defined(_WIN32)
  return true;
#else
  return false;
#endif
}

bool can_detect_huge_pages_configuration() {
#ifdef __linux__
  return true;
#else
  return false;
#endif
}

bool huge_pages_configured() {
#ifdef __linux__
  std::ifstream in("/proc/sys/vm/nr_hugepages");
  if (!in) {
    return false;
  }
  uint64_t pages = 0;
  in >> pages;
  return pages >= 2000;
#elif defined(_WIN32)
  return GetLargePageMinimum() > 0 && windows_large_page_privilege_enabled();
#else
  return false;
#endif
}

bool numa_detected() {
#ifdef __linux__
#ifdef SCRIG_HAVE_LIBNUMA
  if (numa_available() == 0 && numa_max_node() > 0) {
    return true;
  }
#endif
  std::ifstream in("/sys/devices/system/node/online");
  if (!in) {
    return false;
  }
  std::string topology;
  in >> topology;
  return topology.find('-') != std::string::npos || topology.find(',') != std::string::npos;
#else
  return false;
#endif
}

std::string platform_tuning_summary() {
#ifdef __linux__
  return std::string("linux") +
         " huge_pages=" + (huge_pages_configured() ? "on" : "off") +
         " numa=" + (numa_detected() ? "yes" : "no") +
         " affinity=" + affinity_plan_source();
#elif defined(_WIN32)
  return "windows huge_pages=manual numa=auto affinity=" + affinity_plan_source();
#elif defined(__APPLE__)
  return "macos huge_pages=n/a numa=n/a affinity=" + affinity_plan_source();
#else
  return "unknown";
#endif
}

} // namespace scrig
