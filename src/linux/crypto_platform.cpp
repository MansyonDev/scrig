#include "scrig/crypto_platform.hpp"

namespace scrig {

bool crypto_hard_aes_runtime_probe_supported() {
  return false;
}

bool crypto_hard_aes_runtime_probe() {
  return false;
}

std::string crypto_hard_aes_diagnostics(bool) {
  return {};
}

bool crypto_supports_large_pages_hint() {
  return true;
}

} // namespace scrig
