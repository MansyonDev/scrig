#include "scrig/config.hpp"
#include "scrig/consensus.hpp"
#include "scrig/miner.hpp"
#include "scrig/node_client.hpp"
#include "scrig/perf.hpp"
#include "scrig/types.hpp"
#include "scrig/ui.hpp"

#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

scrig::Miner* g_miner = nullptr;

void handle_signal(int) {
  if (g_miner != nullptr) {
    g_miner->request_stop();
  }
}

[[noreturn]] void force_exit_now() {
#ifdef _WIN32
  (void)TerminateProcess(GetCurrentProcess(), 0);
#endif
  std::_Exit(0);
}

bool maybe_enable_ansi() {
#ifdef _WIN32
  HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h_out == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD mode = 0;
  if (!GetConsoleMode(h_out, &mode)) {
    return false;
  }

  const DWORD requested = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
  if (!SetConsoleMode(h_out, requested)) {
    if (!SetConsoleMode(h_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
      return false;
    }
  }
  return true;
#else
  return true;
#endif
}

class QuitHotkeyWatcher {
public:
  ~QuitHotkeyWatcher() {
    stop();
  }

  bool start(scrig::Miner& miner, std::string* error_message = nullptr) {
    miner_ = &miner;
    running_.store(true, std::memory_order_relaxed);

#ifdef _WIN32
    (void)error_message;
    HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    if (h_in != INVALID_HANDLE_VALUE) {
      (void)FlushConsoleInputBuffer(h_in);
    }
    worker_ = std::thread([this]() { run_windows(); });
    return true;
#else
    tty_fd_ = ::open("/dev/tty", O_RDONLY);
    bool using_stdin_dup = false;
    if (tty_fd_ < 0 && isatty(STDIN_FILENO)) {
      tty_fd_ = ::dup(STDIN_FILENO);
      using_stdin_dup = true;
    }
    if (tty_fd_ < 0) {
      running_.store(false, std::memory_order_relaxed);
      if (error_message != nullptr) {
        *error_message = "failed to open terminal input device";
      }
      return false;
    }

    if (!enable_unix_raw_mode()) {
      ::close(tty_fd_);
      tty_fd_ = -1;
      if (!using_stdin_dup && isatty(STDIN_FILENO)) {
        tty_fd_ = ::dup(STDIN_FILENO);
        using_stdin_dup = true;
      }
      if (tty_fd_ < 0 || !enable_unix_raw_mode()) {
        if (tty_fd_ >= 0) {
          ::close(tty_fd_);
          tty_fd_ = -1;
        }
        running_.store(false, std::memory_order_relaxed);
        if (error_message != nullptr) {
          *error_message = "failed to enable raw terminal mode";
        }
        return false;
      }
    }
    worker_ = std::thread([this]() { run_unix(); });
    return true;
#endif
  }

  void stop() {
    running_.store(false, std::memory_order_relaxed);
    if (worker_.joinable()) {
      worker_.join();
    }
#ifndef _WIN32
    disable_unix_raw_mode();
    if (tty_fd_ >= 0) {
      ::close(tty_fd_);
      tty_fd_ = -1;
    }
#endif
  }

private:
  void trigger_stop() {
    if (miner_ != nullptr) {
      miner_->request_stop();
    }
    scrig::NodeClient::interrupt_all();
    running_.store(false, std::memory_order_relaxed);
  }

  void handle_hotkey(unsigned char ch) {
    if (ch == 17) { // Ctrl+Q
      trigger_stop();
      force_exit_now();
    }

    if (ch == static_cast<unsigned char>('q') || ch == static_cast<unsigned char>('Q')) {
      trigger_stop();
      force_exit_now();
    }

    if (miner_ == nullptr) {
      return;
    }

    if (ch == static_cast<unsigned char>('h') || ch == static_cast<unsigned char>('H')) {
      miner_->report_hashrate_now();
      return;
    }
    if (ch == static_cast<unsigned char>('p') || ch == static_cast<unsigned char>('P')) {
      miner_->request_pause();
      return;
    }
    if (ch == static_cast<unsigned char>('r') || ch == static_cast<unsigned char>('R')) {
      miner_->request_resume();
      return;
    }
  }

#ifdef _WIN32
  void run_windows() {
    while (running_.load(std::memory_order_relaxed)) {
      if (_kbhit() == 0) {
        Sleep(25);
        continue;
      }

      const int raw = _getch();
      if (raw == 0 || raw == 224) {
        // Extended key prefix, consume next byte and ignore.
        if (_kbhit() != 0) {
          (void)_getch();
        }
        continue;
      }

      if (raw == 3 || raw == 17) {
        trigger_stop();
        return;
      }

      handle_hotkey(static_cast<unsigned char>(raw));
      if (!running_.load(std::memory_order_relaxed)) {
        return;
      }
    }
  }
#else
  bool enable_unix_raw_mode() {
    if (raw_mode_enabled_) {
      return true;
    }
    if (tty_fd_ < 0) {
      return false;
    }
    if (tcgetattr(tty_fd_, &saved_termios_) != 0) {
      return false;
    }

    termios raw = saved_termios_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(tty_fd_, TCSANOW, &raw) != 0) {
      return false;
    }

    raw_mode_enabled_ = true;
    return true;
  }

  void disable_unix_raw_mode() {
    if (!raw_mode_enabled_) {
      return;
    }
    if (tty_fd_ >= 0) {
      (void)tcsetattr(tty_fd_, TCSANOW, &saved_termios_);
    }
    raw_mode_enabled_ = false;
  }

  void run_unix() {
    while (running_.load(std::memory_order_relaxed)) {
      if (tty_fd_ < 0) {
        return;
      }
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(tty_fd_, &readfds);
      timeval timeout{};
      timeout.tv_sec = 0;
      timeout.tv_usec = 100000;

      const int ready = select(tty_fd_ + 1, &readfds, nullptr, nullptr, &timeout);
      if (ready <= 0 || !FD_ISSET(tty_fd_, &readfds)) {
        continue;
      }

      unsigned char ch = 0;
      const ssize_t n = ::read(tty_fd_, &ch, 1);
      if (n != 1) {
        continue;
      }

      handle_hotkey(ch);
      if (!running_.load(std::memory_order_relaxed)) {
        return;
      }
    }
  }
#endif

  scrig::Miner* miner_ = nullptr;
  std::atomic<bool> running_{false};
  std::thread worker_{};
#ifndef _WIN32
  bool raw_mode_enabled_ = false;
  int tty_fd_ = -1;
  termios saved_termios_{};
#endif
};

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
    config.threads = scrig::recommended_mining_threads();
    if (physical > 0) {
      notes.push_back(
        "threads was 0; using recommended mining threads (" + std::to_string(config.threads) +
        ", physical=" + std::to_string(physical) +
        ", logical=" + std::to_string(logical) + ")");
    } else {
      notes.push_back("threads was 0; using logical CPU count (" + std::to_string(config.threads) + ")");
    }
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

#if defined(__APPLE__)
  if (config.randomx_jit && !config.randomx_macos_unsafe) {
    config.randomx_jit = false;
    notes.push_back("randomx_jit disabled on macOS stability profile (set randomx_macos_unsafe=true to force jit)");
  }
  if (config.randomx_full_mem && !config.randomx_macos_unsafe) {
    config.randomx_full_mem = false;
    notes.push_back("randomx_full_mem disabled on macOS stability profile (set randomx_macos_unsafe=true to force full_mem)");
  }
#endif

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
    const bool ansi_enabled = maybe_enable_ansi();
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
    QuitHotkeyWatcher hotkey_watcher;
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
