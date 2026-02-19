#include "scrig/perf_platform.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <pthread.h>
#include <sched.h>

#ifdef SCRIG_HAVE_LIBNUMA
#include <numa.h>
#endif

namespace scrig {

namespace {

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

} // namespace

bool os_build_affinity_plan(AffinityPlanData* plan) {
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
      if (record.core_type >= 2) {
        slot.performance_tier = 0;
      } else if (record.core_type == 1) {
        slot.performance_tier = 1;
      } else {
        slot.performance_tier = 0;
      }
    } else if (max_seen_freq > 0) {
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

bool os_pin_current_thread(const AffinitySlot& slot) {
  if (slot.os_cpu >= CPU_SETSIZE) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(slot.os_cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &set) == 0;
}

bool os_bind_current_thread_numa(uint32_t worker_index, uint32_t) {
#ifdef SCRIG_HAVE_LIBNUMA
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
  (void)worker_index;
  return false;
#endif
}

bool os_apply_mining_thread_priority(uint32_t, const AffinitySlot*, bool) {
  sched_param param{};
  param.sched_priority = 0;
  return pthread_setschedparam(pthread_self(), SCHED_BATCH, &param) == 0;
}

bool os_thread_pinning_supported() {
  return true;
}

bool os_numa_binding_supported() {
#ifdef SCRIG_HAVE_LIBNUMA
  return numa_available() == 0;
#else
  return false;
#endif
}

bool os_huge_pages_supported_on_platform() {
  return true;
}

bool os_can_detect_huge_pages_configuration() {
  return true;
}

bool os_huge_pages_configured() {
  std::ifstream in("/proc/sys/vm/nr_hugepages");
  if (!in) {
    return false;
  }
  uint64_t pages = 0;
  in >> pages;
  return pages >= 2000;
}

bool os_numa_detected() {
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
}

std::string os_platform_tuning_summary(const std::string& affinity_source) {
  return std::string("linux") +
         " huge_pages=" + (os_huge_pages_configured() ? "on" : "off") +
         " numa=" + (os_numa_detected() ? "yes" : "no") +
         " affinity=" + affinity_source;
}

} // namespace scrig
