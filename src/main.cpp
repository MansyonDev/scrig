#include "scrig/config.hpp"
#include "scrig/consensus.hpp"
#include "scrig/miner.hpp"
#include "scrig/node_client.hpp"
#include "scrig/perf.hpp"
#include "scrig/platform.hpp"
#include "scrig/runtime_platform.hpp"
#include "scrig/types.hpp"
#include "scrig/ui.hpp"

#include <algorithm>
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

scrig::Miner* g_miner = nullptr;

void handle_signal(int) {
  if (g_miner != nullptr) {
    g_miner->request_stop();
  }
}

struct CliOptions {
  std::filesystem::path config_path = "config.json";
  bool validate_only = false;
};

CliOptions parse_cli(int argc, char** argv) {
  CliOptions opts;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) {
        throw std::runtime_error("--config requires a path");
      }
      opts.config_path = argv[++i];
      continue;
    }

    if (arg == "--validate-node") {
      opts.validate_only = true;
      continue;
    }

    if (arg == "--help" || arg == "-h") {
      std::cout << "scrig miner\n\n"
                << "Usage:\n"
                << "  scrig [--config <path>] [--validate-node]\n\n"
                << "Options:\n"
                << "  --config <path>   Path to config JSON (default: ./config.json)\n"
                << "  --validate-node   Validate node/pool connectivity and exit\n"
                << "  -h, --help        Show this help\n";
      std::exit(0);
    }
  }

  return opts;
}

std::vector<std::string> sanitize_runtime_config(scrig::Config& config) {
  std::vector<std::string> notes;
  notes.push_back("cpu profile: " + scrig::cpu_runtime_summary());

  if (config.threads == 0) {
    const auto logical = scrig::logical_cpu_count();
    const auto physical = scrig::physical_cpu_count();
    config.threads = scrig::recommended_mining_threads(config.performance_cores_only);
    if (physical > 0) {
      notes.push_back(
        "threads was 0; using recommended mining threads (" + std::to_string(config.threads) +
        ", physical=" + std::to_string(physical) +
        ", logical=" + std::to_string(logical) + ")");
    } else {
      notes.push_back("threads was 0; using logical CPU count (" + std::to_string(config.threads) + ")");
    }
  }

  if (config.performance_cores_only) {
    if (!scrig::hybrid_topology_detected()) {
      notes.push_back("performance_cores_only requested but no hybrid topology detected; using all cores");
      config.performance_cores_only = false;
    } else {
      const auto perf_logical = scrig::performance_core_logical_count();
      if (config.threads > perf_logical) {
        notes.push_back(
          "threads capped to performance-core logical count (" + std::to_string(perf_logical) +
          ") due to performance_cores_only");
        config.threads = std::max<uint32_t>(1U, perf_logical);
      }
    }
  }

  if (config.randomx_pipeline_batch > 0) {
    notes.push_back("randomx_pipeline_batch preset=" + std::to_string(config.randomx_pipeline_batch));
  }
  if (config.auto_tune_startup) {
    notes.push_back(
      "startup auto-tune enabled: duration=" + std::to_string(config.auto_tune_seconds) + "s");
  }

  if (config.pin_threads && !scrig::thread_pinning_supported()) {
    config.pin_threads = false;
    notes.push_back("pin_threads disabled: CPU affinity is not supported on this platform");
  } else if (config.pin_threads) {
    notes.push_back("affinity profile: " + scrig::affinity_profile_summary(config.threads, 10));
  }

  if (config.numa_bind && !scrig::numa_binding_supported()) {
    config.numa_bind = false;
    notes.push_back("numa_bind disabled: NUMA binding is unavailable on this platform/build");
  } else if (config.numa_bind && !scrig::numa_detected()) {
    config.numa_bind = false;
    notes.push_back("numa_bind disabled: no multi-node NUMA topology detected");
  }

  if (config.randomx_huge_pages && !scrig::huge_pages_supported_on_platform()) {
    config.randomx_huge_pages = false;
    notes.push_back("randomx_huge_pages disabled: huge pages are not supported on this platform");
  } else if (config.randomx_huge_pages &&
             scrig::can_detect_huge_pages_configuration() &&
             !scrig::huge_pages_configured()) {
    notes.push_back("randomx_huge_pages requested: host precheck reports not configured; runtime will retry/fallback");
  }

  scrig::apply_platform_runtime_safety(config, notes);

  return notes;
}

void validate_node(const scrig::Config& config) {
  const auto wallet = scrig::public_key_from_base36(config.wallet_address);

  scrig::NodeClient node(config.node_host, config.node_port);
  node.connect();

  const auto height = node.height();
  const auto diff = node.difficulty();
  const auto reward = node.reward();
  const auto latest_hash = height == 0 ? std::optional<scrig::Hash>{} : node.block_hash(height - 1);

  std::cout << "Node validation OK\n";
  std::cout << "  endpoint: " << config.node_host << ':' << config.node_port << '\n';
  std::cout << "  height: " << height << '\n';
  std::cout << "  reward: " << reward << " nano\n";
  std::cout << "  block_difficulty[0..3]: "
            << static_cast<int>(diff.block_difficulty[0]) << ','
            << static_cast<int>(diff.block_difficulty[1]) << ','
            << static_cast<int>(diff.block_difficulty[2]) << ','
            << static_cast<int>(diff.block_difficulty[3]) << '\n';

  if (latest_hash.has_value()) {
    std::cout << "  latest block hash: " << scrig::hash_to_base36(*latest_hash) << '\n';
  }

  if (config.mode == "pool") {
    scrig::NodeClient pool(config.pool_host, config.pool_port);
    pool.connect();
    const auto pool_diff = pool.initialize_pool_handshake(wallet);
    std::cout << "Pool validation OK\n";
    std::cout << "  endpoint: " << config.pool_host << ':' << config.pool_port << '\n';
    std::cout << "  pool_difficulty[0..3]: "
              << static_cast<int>(pool_diff[0]) << ','
              << static_cast<int>(pool_diff[1]) << ','
              << static_cast<int>(pool_diff[2]) << ','
              << static_cast<int>(pool_diff[3]) << '\n';
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    const bool ansi_enabled = scrig::enable_console_ansi();
    scrig::set_dashboard_ansi_enabled(ansi_enabled);

    const auto cli = parse_cli(argc, argv);

    bool created_default = false;
    auto config = scrig::load_or_create_config(cli.config_path, created_default);

    if (created_default) {
      std::cout << "Created default config at: " << cli.config_path << '\n';
      std::cout << "Set wallet_address in that file, then run scrig again." << '\n';
      return 0;
    }

    const auto tuning_notes = sanitize_runtime_config(config);
    const auto hashing_caps = scrig::hashing_capability_profile();
    scrig::save_config(cli.config_path, config);
    scrig::validate_config(config);

    if (cli.validate_only) {
      validate_node(config);
      return 0;
    }

    if (!config.dashboard) {
      std::cout << "Starting scrig " << SCRIG_VERSION
                << " | mode=" << config.mode
                << " | node=" << config.node_host << ':' << config.node_port
                << " | threads=" << config.threads << '\n';
      std::cout << "Tuning: " << scrig::platform_tuning_summary() << '\n';
      for (const auto& note : tuning_notes) {
        std::cout << "Auto-tuning: " << note << '\n';
      }
      if (config.randomx_huge_pages &&
          scrig::huge_pages_supported_on_platform() &&
          scrig::can_detect_huge_pages_configuration() &&
          !scrig::huge_pages_configured()) {
        std::cout << "Notice: huge pages are not configured on this OS profile; RandomX may run slower." << '\n';
      }
    }

    scrig::Miner miner(std::move(config));
    scrig::QuitHotkeyWatcher hotkey_watcher;
    g_miner = &miner;
    std::signal(SIGINT, handle_signal);
#ifndef _WIN32
    std::signal(SIGTERM, handle_signal);
#endif
    std::string hotkey_error;
    const bool hotkey_enabled = hotkey_watcher.start(miner, &hotkey_error);
    if (hotkey_enabled) {
      miner.add_runtime_note("Controls: h=hashrate, p=pause, r=resume, q=quit");
    } else {
      miner.add_runtime_note("Hotkey unavailable: " + hotkey_error + ". Use Ctrl+C to quit.");
    }
    for (const auto& note : tuning_notes) {
      miner.add_runtime_note("Auto-tuning: " + note);
    }
    if (!ansi_enabled) {
      miner.add_runtime_note("Dashboard ANSI mode unavailable; using compatibility redraw");
    }
    if (hashing_caps.randomx) {
      miner.add_runtime_note(
        "RandomX capabilities: jit=" + std::string(hashing_caps.jit ? "on" : "off") +
        " hard_aes=" + std::string(hashing_caps.hard_aes ? "on" : "off") +
        " argon2_avx2=" + std::string(hashing_caps.argon2_avx2 ? "on" : "off") +
        " argon2_ssse3=" + std::string(hashing_caps.argon2_ssse3 ? "on" : "off"));
      if (!hashing_caps.hard_aes) {
        miner.add_runtime_note("RandomX capability: HARD_AES unavailable on this runtime/build");
      }
      if (!hashing_caps.jit) {
        miner.add_runtime_note("RandomX capability: JIT unavailable on this runtime/build");
      }
    }
    if (!config.dashboard) {
      if (hotkey_enabled) {
        std::cout << "Controls: h=hashrate, p=pause, r=resume, q=quit" << '\n';
      } else {
        std::cout << "Hotkey unavailable: " << hotkey_error << ". Use Ctrl+C to quit." << '\n';
      }
    }

    miner.run();
    g_miner = nullptr;

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "fatal: " << ex.what() << '\n';
    return 1;
  }
}
