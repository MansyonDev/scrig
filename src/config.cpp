#include "scrig/config.hpp"

#include "scrig/json.hpp"
#include "scrig/types.hpp"

#include <fstream>
#include <stdexcept>
#include <thread>

namespace scrig {

namespace {

Config default_config() {
  Config config;
  const auto hw = std::thread::hardware_concurrency();
  config.threads = hw == 0 ? 1U : hw;
  return config;
}

JsonValue config_to_json(const Config& config) {
  JsonValue::object root{
    {"wallet_address", JsonValue(config.wallet_address)},
    {"node_host", JsonValue(config.node_host)},
    {"node_port", JsonValue(static_cast<uint64_t>(config.node_port))},
    {"mode", JsonValue(config.mode)},
    {"pool_host", JsonValue(config.pool_host)},
    {"pool_port", JsonValue(static_cast<uint64_t>(config.pool_port))},
    {"threads", JsonValue(static_cast<uint64_t>(config.threads))},
    {"include_mempool_transactions", JsonValue(config.include_mempool_transactions)},
    {"refresh_interval_ms", JsonValue(config.refresh_interval_ms)},
    {"use_chain_events", JsonValue(config.use_chain_events)},
    {"colorful_ui", JsonValue(config.colorful_ui)},
    {"dashboard", JsonValue(config.dashboard)},
    {"pin_threads", JsonValue(config.pin_threads)},
    {"numa_bind", JsonValue(config.numa_bind)},
    {"randomx_full_mem", JsonValue(config.randomx_full_mem)},
    {"randomx_huge_pages", JsonValue(config.randomx_huge_pages)},
    {"randomx_jit", JsonValue(config.randomx_jit)},
    {"randomx_hard_aes", JsonValue(config.randomx_hard_aes)},
    {"randomx_secure", JsonValue(config.randomx_secure)},
    {"randomx_macos_unsafe", JsonValue(config.randomx_macos_unsafe)},
    {"_note", JsonValue("Set wallet_address before mining.")},
  };
  return JsonValue(std::move(root));
}

Config config_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    throw std::runtime_error("config root must be an object");
  }

  Config config = default_config();
  const auto& obj = value.as_object();

  if (const auto it = obj.find("wallet_address"); it != obj.end()) config.wallet_address = it->second.as_string();
  if (const auto it = obj.find("node_host"); it != obj.end()) config.node_host = it->second.as_string();
  if (const auto it = obj.find("node_port"); it != obj.end()) config.node_port = static_cast<uint16_t>(it->second.as_uint64());
  if (const auto it = obj.find("mode"); it != obj.end()) config.mode = it->second.as_string();
  if (const auto it = obj.find("pool_host"); it != obj.end()) config.pool_host = it->second.as_string();
  if (const auto it = obj.find("pool_port"); it != obj.end()) config.pool_port = static_cast<uint16_t>(it->second.as_uint64());
  if (const auto it = obj.find("threads"); it != obj.end()) config.threads = static_cast<uint32_t>(it->second.as_uint64());
  if (const auto it = obj.find("include_mempool_transactions"); it != obj.end()) config.include_mempool_transactions = it->second.as_bool();
  if (const auto it = obj.find("refresh_interval_ms"); it != obj.end()) config.refresh_interval_ms = it->second.as_uint64();
  if (const auto it = obj.find("use_chain_events"); it != obj.end()) config.use_chain_events = it->second.as_bool();
  if (const auto it = obj.find("colorful_ui"); it != obj.end()) config.colorful_ui = it->second.as_bool();
  if (const auto it = obj.find("dashboard"); it != obj.end()) config.dashboard = it->second.as_bool();
  if (const auto it = obj.find("pin_threads"); it != obj.end()) config.pin_threads = it->second.as_bool();
  if (const auto it = obj.find("numa_bind"); it != obj.end()) config.numa_bind = it->second.as_bool();
  if (const auto it = obj.find("randomx_full_mem"); it != obj.end()) config.randomx_full_mem = it->second.as_bool();
  if (const auto it = obj.find("randomx_huge_pages"); it != obj.end()) config.randomx_huge_pages = it->second.as_bool();
  if (const auto it = obj.find("randomx_jit"); it != obj.end()) config.randomx_jit = it->second.as_bool();
  if (const auto it = obj.find("randomx_hard_aes"); it != obj.end()) config.randomx_hard_aes = it->second.as_bool();
  if (const auto it = obj.find("randomx_secure"); it != obj.end()) config.randomx_secure = it->second.as_bool();
  if (const auto it = obj.find("randomx_macos_unsafe"); it != obj.end()) config.randomx_macos_unsafe = it->second.as_bool();

  if (config.threads == 0) {
    const auto hw = std::thread::hardware_concurrency();
    config.threads = hw == 0 ? 1U : hw;
  }

  return config;
}

} // namespace

Config load_or_create_config(const std::filesystem::path& path, bool& created_default) {
  created_default = false;

  if (!std::filesystem::exists(path)) {
    created_default = true;
    const Config config = default_config();
    std::ofstream out(path);
    if (!out) {
      throw std::runtime_error("failed to create config file: " + path.string());
    }
    out << to_json(config_to_json(config), true) << '\n';
    return config;
  }

  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to read config file: " + path.string());
  }

  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return config_from_json(parse_json(text));
}

void validate_config(const Config& config) {
  if (config.wallet_address.empty() || config.wallet_address == "<YOUR_PUBLIC_WALLET_ADDRESS_BASE36>") {
    throw std::runtime_error("config wallet_address is missing");
  }
  try {
    (void)public_key_from_base36(config.wallet_address);
  } catch (...) {
    throw std::runtime_error("config wallet_address is not valid base36");
  }
  if (config.node_host.empty()) {
    throw std::runtime_error("config node_host is missing");
  }
  if (config.node_port == 0) {
    throw std::runtime_error("config node_port must be > 0");
  }
  if (config.pool_host.empty()) {
    throw std::runtime_error("config pool_host is missing");
  }
  if (config.pool_port == 0) {
    throw std::runtime_error("config pool_port must be > 0");
  }
  if (config.threads == 0) {
    throw std::runtime_error("config threads must be > 0");
  }
  if (config.mode != "solo" && config.mode != "pool") {
    throw std::runtime_error("config mode must be 'solo' or 'pool'");
  }
}

} // namespace scrig
