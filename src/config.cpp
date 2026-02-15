#include "scrig/config.hpp"

#include "scrig/json.hpp"
#include "scrig/perf.hpp"
#include "scrig/types.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace scrig {

namespace {

Config default_config() {
  Config config;
  config.threads = 0;
  config.pin_threads = thread_pinning_supported();
  config.numa_bind = false;
  config.randomx_huge_pages = huge_pages_supported_on_platform();
  config.use_chain_events = true;
  config.randomx_hard_aes = true;
  config.randomx_secure = false;
  config.randomx_macos_unsafe = false;
  config.include_mempool_transactions = false;
  config.refresh_interval_ms = 500;
  config.randomx_jit = true;
  config.randomx_full_mem = true;

#if defined(__APPLE__)
  config.randomx_huge_pages = false;
  config.randomx_macos_unsafe = true;
#elif defined(_WIN32)
  config.numa_bind = false;
#elif defined(__linux__)
  config.numa_bind = numa_binding_supported() && numa_detected();
#endif

  return config;
}

const char* platform_profile_name() {
#if defined(__APPLE__)
  return "macos-performance";
#elif defined(_WIN32)
  return "windows-performance";
#elif defined(__linux__)
  return "linux-performance";
#else
  return "generic";
#endif
}

const char* platform_config_comment() {
#if defined(__APPLE__)
  return "macOS Specific Config";
#elif defined(_WIN32)
  return "Windows Specific Config";
#elif defined(__linux__)
  return "Linux Specific Config";
#else
  return "Generic OS Config";
#endif
}

bool include_pin_threads_key() {
  return thread_pinning_supported();
}

bool include_numa_bind_key() {
  return numa_binding_supported();
}

bool include_randomx_huge_pages_key() {
  return huge_pages_supported_on_platform();
}

bool include_randomx_macos_unsafe_key() {
#if defined(__APPLE__)
  return true;
#else
  return false;
#endif
}

std::string json_scalar(const std::string& value) {
  return to_json(JsonValue(value), false);
}

const char* json_bool(bool value) {
  return value ? "true" : "false";
}

void write_field(std::ostringstream& out,
                 const std::string& key,
                 const std::string& value,
                 bool comma = true) {
  out << "  " << json_scalar(key) << ": " << value;
  if (comma) {
    out << ',';
  }
  out << '\n';
}

std::string config_to_json_text(const Config& config) {
  std::ostringstream out;
  out << "{\n";

  write_field(out, "_config_comment", json_scalar(platform_config_comment()));
  write_field(out, "_profile", json_scalar(platform_profile_name()));
  write_field(out, "_note", json_scalar("Set wallet_address before mining."));
  out << '\n';

  write_field(out, "wallet_address", json_scalar(config.wallet_address));
  out << '\n';

  write_field(out, "mode", json_scalar(config.mode));
  out << '\n';

  write_field(out, "node_host", json_scalar(config.node_host));
  write_field(out, "node_port", std::to_string(config.node_port));
  write_field(out, "pool_host", json_scalar(config.pool_host));
  write_field(out, "pool_port", std::to_string(config.pool_port));
  out << '\n';

  write_field(out, "threads", std::to_string(config.threads));
  write_field(out, "include_mempool_transactions", json_bool(config.include_mempool_transactions));
  write_field(out, "refresh_interval_ms", std::to_string(config.refresh_interval_ms));
  write_field(out, "use_chain_events", json_bool(config.use_chain_events));
  out << '\n';

  if (include_pin_threads_key()) {
    write_field(out, "pin_threads", json_bool(config.pin_threads));
  }
  if (include_numa_bind_key()) {
    write_field(out, "numa_bind", json_bool(config.numa_bind));
  }
  write_field(out, "randomx_full_mem", json_bool(config.randomx_full_mem));
  if (include_randomx_huge_pages_key()) {
    write_field(out, "randomx_huge_pages", json_bool(config.randomx_huge_pages));
  }
  write_field(out, "randomx_jit", json_bool(config.randomx_jit));
  write_field(out, "randomx_hard_aes", json_bool(config.randomx_hard_aes));
  write_field(out, "randomx_secure", json_bool(config.randomx_secure));
  if (include_randomx_macos_unsafe_key()) {
    write_field(out, "randomx_macos_unsafe", json_bool(config.randomx_macos_unsafe));
  }
  out << '\n';

  write_field(out, "colorful_ui", json_bool(config.colorful_ui));
  write_field(out, "dashboard", json_bool(config.dashboard), false);
  out << "}";

  return out.str();
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
    out << config_to_json_text(config) << '\n';
    return config;
  }

  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to read config file: " + path.string());
  }

  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto config = config_from_json(parse_json(text));

  // Keep config deterministic and grouped for easier manual editing.
  std::ofstream out(path);
  if (out) {
    out << config_to_json_text(config) << '\n';
  }

  return config;
}

void save_config(const std::filesystem::path& path, const Config& config) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to write config file: " + path.string());
  }
  out << config_to_json_text(config) << '\n';
}

void validate_config(const Config& config) {
  if (config.wallet_address.empty() ||
      config.wallet_address == "<PUT_YOUR_WALLET_ADDRESS>" ||
      config.wallet_address == "<YOUR_PUBLIC_WALLET_ADDRESS_BASE36>") {
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
