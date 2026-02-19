#include "scrig/crypto_platform.hpp"

#include <cstring>
#include <immintrin.h>
#include <intrin.h>
#include <sstream>
#include <string>

#include <windows.h>

namespace scrig {

namespace {

struct WindowsCpuDiag {
  bool aes = false;
  bool avx2 = false;
  bool ssse3 = false;
  bool hypervisor_present = false;
  std::string vendor;
  std::string hypervisor_vendor;
};

WindowsCpuDiag windows_cpu_diag() {
  WindowsCpuDiag diag{};
  int info[4] = {0, 0, 0, 0};

  __cpuid(info, 0);
  const int max_leaf = info[0];
  char vendor[13] = {};
  std::memcpy(vendor + 0, &info[1], 4);
  std::memcpy(vendor + 4, &info[3], 4);
  std::memcpy(vendor + 8, &info[2], 4);
  vendor[12] = '\0';
  diag.vendor = vendor;

  if (max_leaf >= 1) {
    __cpuid(info, 1);
    diag.aes = (info[2] & (1 << 25)) != 0;
    diag.ssse3 = (info[2] & (1 << 9)) != 0;
    diag.hypervisor_present = (info[2] & (1u << 31)) != 0;
  }

  if (max_leaf >= 7) {
    __cpuidex(info, 7, 0);
    diag.avx2 = (info[1] & (1 << 5)) != 0;
  }

  if (diag.hypervisor_present) {
    __cpuid(info, 0x40000000);
    char hv_vendor[13] = {};
    std::memcpy(hv_vendor + 0, &info[1], 4);
    std::memcpy(hv_vendor + 4, &info[2], 4);
    std::memcpy(hv_vendor + 8, &info[3], 4);
    hv_vendor[12] = '\0';
    diag.hypervisor_vendor = hv_vendor;
  }

  return diag;
}

} // namespace

bool crypto_hard_aes_runtime_probe_supported() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86))
  return true;
#else
  return false;
#endif
}

bool crypto_hard_aes_runtime_probe() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86))
  __try {
    __m128i state = _mm_setzero_si128();
    const __m128i key = _mm_set_epi32(0xA5A5A5A5, 0xC3C3C3C3, 0x5A5A5A5A, 0x3C3C3C3C);
    state = _mm_aesenc_si128(state, key);
    volatile int sink = _mm_cvtsi128_si32(state);
    (void)sink;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#else
  return false;
#endif
}

std::string crypto_hard_aes_diagnostics(bool probe_ok) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86))
  const auto diag = windows_cpu_diag();
  std::ostringstream out;
  out << "[RANDOMX] Windows AES diagnostics: cpuid_aes="
      << (diag.aes ? "1" : "0")
      << " ssse3=" << (diag.ssse3 ? "1" : "0")
      << " avx2=" << (diag.avx2 ? "1" : "0")
      << " hypervisor=" << (diag.hypervisor_present ? "1" : "0")
      << " vendor=" << diag.vendor;
  if (diag.hypervisor_present && !diag.hypervisor_vendor.empty()) {
    out << " hypervisor_vendor=" << diag.hypervisor_vendor;
  }
  out << " runtime_probe=" << (probe_ok ? "pass" : "fail");
  return out.str();
#else
  (void)probe_ok;
  return {};
#endif
}

bool crypto_supports_large_pages_hint() {
  return true;
}

} // namespace scrig
