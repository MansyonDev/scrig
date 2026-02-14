#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace scrig {

struct Config {
  std::string wallet_address = "<YOUR_PUBLIC_WALLET_ADDRESS_BASE36>";

  std::string node_host = "127.0.0.1";
  uint16_t node_port = 3003;

  std::string mode = "solo"; // solo | pool
  std::string pool_host = "127.0.0.1";
  uint16_t pool_port = 3003;

  uint32_t threads = 0;
  bool include_mempool_transactions = true;
  uint64_t refresh_interval_ms = 500;
  bool use_chain_events = true;

  bool colorful_ui = true;
  bool dashboard = true;
  bool pin_threads = true;
  bool numa_bind = false;

  bool randomx_full_mem = true;
  bool randomx_huge_pages = true;
  bool randomx_jit = true;
  bool randomx_hard_aes = true;
  bool randomx_secure = false;
  bool randomx_macos_unsafe = false;
};

Config load_or_create_config(const std::filesystem::path& path, bool& created_default);
void validate_config(const Config& config);

} // namespace scrig
