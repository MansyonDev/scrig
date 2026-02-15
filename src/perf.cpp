#include "scrig/perf.hpp"

#include <thread>

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
#endif

namespace scrig {

#ifdef _WIN32
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

uint32_t logical_cpu_count() {
  const auto hw = std::thread::hardware_concurrency();
  return hw == 0 ? 1U : hw;
}

bool pin_current_thread(uint32_t worker_index, uint32_t worker_count) {
  (void)worker_count;

  const uint32_t cpus = logical_cpu_count();
  if (cpus == 0) {
    return false;
  }

  [[maybe_unused]] const uint32_t cpu_index = worker_index % cpus;

#ifdef _WIN32
  const DWORD_PTR mask = (static_cast<DWORD_PTR>(1) << cpu_index);
  return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu_index, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &set) == 0;
#elif defined(__APPLE__)
  // macOS does not provide strict CPU pinning, but affinity tags help scheduler locality.
  thread_affinity_policy_data_t policy{};
  policy.affinity_tag = static_cast<integer_t>(cpu_index + 1U);
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
         " numa=" + (numa_detected() ? "yes" : "no");
#elif defined(_WIN32)
  return "windows huge_pages=manual numa=auto";
#elif defined(__APPLE__)
  return "macos huge_pages=n/a numa=n/a";
#else
  return "unknown";
#endif
}

} // namespace scrig
