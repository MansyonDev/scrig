#pragma once

#include <string>

namespace scrig {

bool crypto_hard_aes_runtime_probe_supported();
bool crypto_hard_aes_runtime_probe();
std::string crypto_hard_aes_diagnostics(bool probe_ok);
bool crypto_supports_large_pages_hint();

} // namespace scrig
