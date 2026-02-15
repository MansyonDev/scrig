#include "scrig/ui.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace scrig {

namespace {

constexpr size_t kInnerWidth = 108;
constexpr size_t kTopRows = 17;
constexpr size_t kLogHeaderRow = kTopRows + 1;
constexpr size_t kLogSeparatorRow = kTopRows + 2;
constexpr size_t kLogStartRow = kTopRows + 3;
bool g_dashboard_ansi_enabled = true;

std::string colorize(const std::string& text, const char* code, bool enabled) {
  if (!enabled) {
    return text;
  }
  return std::string(code) + text + "\x1b[0m";
}

size_t visible_length(const std::string& text) {
  size_t visible = 0;
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
      i += 2;
      while (i < text.size() && text[i] != 'm') {
        ++i;
      }
      if (i < text.size()) {
        ++i;
      }
      continue;
    }
    ++visible;
    ++i;
  }
  return visible;
}

std::string uptime_string(uint64_t seconds) {
  const auto h = seconds / 3600;
  const auto m = (seconds % 3600) / 60;
  const auto s = seconds % 60;
  std::ostringstream oss;
  oss << h << "h " << m << "m " << s << "s";
  return oss.str();
}

std::string snap_amount(uint64_t nano) {
  constexpr uint64_t kNanoPerSnap = 100000000ULL;
  const uint64_t whole = nano / kNanoPerSnap;
  const uint64_t frac = nano % kNanoPerSnap;
  std::ostringstream oss;
  oss << whole << '.' << std::setw(8) << std::setfill('0') << frac;
  return oss.str();
}

std::string snap_delta(int64_t nano_delta) {
  constexpr int64_t kNanoPerSnap = 100000000LL;
  const char sign = nano_delta < 0 ? '-' : '+';
  const uint64_t abs_nano = static_cast<uint64_t>(nano_delta < 0 ? -nano_delta : nano_delta);
  const uint64_t whole = abs_nano / static_cast<uint64_t>(kNanoPerSnap);
  const uint64_t frac = abs_nano % static_cast<uint64_t>(kNanoPerSnap);
  std::ostringstream oss;
  oss << sign << whole << '.' << std::setw(8) << std::setfill('0') << frac;
  return oss.str();
}

std::string pad_right_visible(const std::string& text, size_t width) {
  const size_t vis = visible_length(text);
  if (vis >= width) {
    return text;
  }
  return text + std::string(width - vis, ' ');
}

std::string clamp_visible(const std::string& text, size_t max_visible) {
  if (visible_length(text) <= max_visible) {
    return text;
  }

  std::string out;
  out.reserve(text.size());

  size_t visible = 0;
  size_t i = 0;
  while (i < text.size() && visible < max_visible) {
    if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
      const size_t esc_start = i;
      i += 2;
      while (i < text.size() && text[i] != 'm') {
        ++i;
      }
      if (i < text.size()) {
        ++i;
      }
      out.append(text.substr(esc_start, i - esc_start));
      continue;
    }

    out.push_back(text[i]);
    ++i;
    ++visible;
  }

  if (max_visible >= 3) {
    if (visible >= 3) {
      while (!out.empty() && visible > max_visible - 3) {
        if (out.back() == '\x1b') {
          break;
        }
        out.pop_back();
        --visible;
      }
      out += "...";
    }
  }

  return out;
}

std::string optimization_state(bool enabled, bool colorful) {
  if (!colorful) {
    return enabled ? "[ON ]" : "[OFF]";
  }
  return enabled
    ? colorize("[ON ]", "\x1b[1;32m", true)
    : colorize("[OFF]", "\x1b[1;31m", true);
}

std::string format_opt_pair(
  const std::string& left_name,
  bool left_enabled,
  const std::string& right_name,
  bool right_enabled,
  bool colorful) {

  const std::string left_state = optimization_state(left_enabled, colorful);
  const std::string right_state = optimization_state(right_enabled, colorful);

  const std::string left = left_name + " " + left_state;
  const std::string right = right_name + " " + right_state;

  return " " + pad_right_visible(left, 51) + " | " + right;
}

std::string command_token(char key, const std::string& suffix, bool colorful) {
  const std::string key_text(1, key);
  const std::string decorated_key = colorful
    ? colorize(key_text, "\x1b[1;33m", true)
    : key_text;
  return "[" + decorated_key + "]" + suffix;
}

std::string box_border() {
  return "+" + std::string(kInnerWidth, '-') + "+";
}

void write_fixed_row(size_t row, const std::string& text) {
  std::cout << "\x1b[" << row << ";1H\x1b[2K" << text;
}

void save_cursor_position() {
  // Use both DEC and CSI save sequences for broader terminal compatibility.
  std::cout << "\x1b" "7\x1b[s";
}

void restore_cursor_position() {
  // Restore in reverse order so either save mechanism can be effective.
  std::cout << "\x1b[u\x1b" "8";
}

#ifdef _WIN32
void clear_console_windows() {
  HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h_out == INVALID_HANDLE_VALUE) {
    return;
  }

  CONSOLE_SCREEN_BUFFER_INFO csbi{};
  if (!GetConsoleScreenBufferInfo(h_out, &csbi)) {
    return;
  }

  const DWORD cell_count = static_cast<DWORD>(csbi.dwSize.X) * static_cast<DWORD>(csbi.dwSize.Y);
  const COORD origin{0, 0};
  DWORD written = 0;
  (void)FillConsoleOutputCharacterA(h_out, ' ', cell_count, origin, &written);
  (void)FillConsoleOutputAttribute(h_out, csbi.wAttributes, cell_count, origin, &written);
  (void)SetConsoleCursorPosition(h_out, origin);
}
#endif

} // namespace

std::string human_hashrate(double rate) {
  static constexpr const char* suffixes[] = {"H/s", "KH/s", "MH/s", "GH/s", "TH/s"};
  size_t suffix_idx = 0;

  while (rate >= 1000.0 && suffix_idx + 1 < (sizeof(suffixes) / sizeof(suffixes[0]))) {
    rate /= 1000.0;
    ++suffix_idx;
  }

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << rate << ' ' << suffixes[suffix_idx];
  return oss.str();
}

void set_dashboard_ansi_enabled(bool enabled) {
  g_dashboard_ansi_enabled = enabled;
}

void render_dashboard(const UiSnapshot& s, bool colorful) {
  static bool initialized = false;
  static bool log_cursor_initialized = false;
  static size_t rendered_log_lines = 0;

  if (g_dashboard_ansi_enabled && !initialized) {
    std::cout << "\x1b[2J";
    initialized = true;
  }

  const std::string title = colorize(" SCRIG - Snap Coin Miner ", "\x1b[1;36m", colorful);
  const std::string donate =
    colorize("Donate the Dev: 2ra6i1cmndm5p2hrjwt0gkb87ydd3uzjj522hpz95qhmct1wkv", "\x1b[1;33m", colorful);
  const std::string status = colorize(" " + s.status + " ", "\x1b[1;32m", colorful);
  std::unordered_map<std::string, bool> opt_map;
  opt_map.reserve(s.optimizations.size());
  for (const auto& opt : s.optimizations) {
    opt_map[opt.name] = opt.enabled;
  }

  std::vector<std::string> rows;
  rows.reserve(kTopRows);
  rows.push_back(box_border());
  rows.push_back("|" + pad_right_visible(title + "  " + donate, kInnerWidth) + "|");
  rows.push_back(box_border());

  rows.push_back("|" + pad_right_visible(
    " Mode: " + s.mode + "    Node: " + s.node, kInnerWidth) + "|");

  rows.push_back("|" + pad_right_visible(" Optimization Profile", kInnerWidth) + "|");
  rows.push_back("|" + pad_right_visible(format_opt_pair("CPU Pinning", opt_map["pin_threads"], "NUMA Binding", opt_map["numa_bind"], colorful), kInnerWidth) + "|");
  rows.push_back("|" + pad_right_visible(format_opt_pair("Chain Events", opt_map["chain_events"], "RandomX JIT", opt_map["rx_jit"], colorful), kInnerWidth) + "|");
  rows.push_back("|" + pad_right_visible(format_opt_pair("RandomX Full Memory", opt_map["rx_full_mem"], "RandomX Huge Pages", opt_map["rx_huge_pages"], colorful), kInnerWidth) + "|");
  rows.push_back("|" + pad_right_visible(format_opt_pair("RandomX Hard AES", opt_map["rx_hard_aes"], "RandomX Secure Mode", opt_map["rx_secure"], colorful), kInnerWidth) + "|");

  rows.push_back("|" + pad_right_visible(
    " Height: " + std::to_string(s.height) +
    "    Threads: " + std::to_string(s.threads), kInnerWidth) + "|");

  rows.push_back("|" + pad_right_visible(
    " Hashrate: " + human_hashrate(s.hashrate) +
    "    Uptime: " + uptime_string(s.uptime_seconds), kInnerWidth) + "|");

  rows.push_back("|" + pad_right_visible(
    (s.mode == "pool"
      ? (" Shares Accepted: " + std::to_string(s.accepted) +
         "    Shares Rejected: " + std::to_string(s.rejected))
      : (" Accepted: " + std::to_string(s.accepted) +
         "    Rejected: " + std::to_string(s.rejected))),
    kInnerWidth) + "|");
  if (s.wallet_balance_available) {
    rows.push_back("|" + pad_right_visible(
      " Wallet (node): " + snap_amount(s.wallet_balance_nano) +
      " SNAP    Session Net: " + snap_delta(s.wallet_delta_nano) + " SNAP",
      kInnerWidth) + "|");
  } else {
    rows.push_back("|" + pad_right_visible(
      " Wallet (node): n/a (node balance unavailable)",
      kInnerWidth) + "|");
  }

  std::string hash_line = s.latest_block_hash;
  if (hash_line.size() > 64) {
    hash_line = hash_line.substr(0, 64);
  }
  rows.push_back("|" + pad_right_visible(" Last Block Hash: " + hash_line, kInnerWidth) + "|");
  rows.push_back("|" + pad_right_visible(" Status: " + status, kInnerWidth) + "|");
  rows.push_back("|" + pad_right_visible(
    " Commands: " +
    command_token('H', "ashrate", colorful) + "  " +
    command_token('P', "ause", colorful) + "  " +
    command_token('R', "esume", colorful) + "  " +
    command_token('Q', "uit", colorful),
    kInnerWidth) + "|");
  rows.push_back(box_border());

  if (!g_dashboard_ansi_enabled) {
#ifdef _WIN32
    clear_console_windows();
#else
    std::cout << "\x1b[2J\x1b[H";
#endif
    for (const auto& row : rows) {
      std::cout << row << '\n';
    }
    std::cout << "Runtime Log (scrollable)\n";
    std::cout << std::string(kInnerWidth, '-') << '\n';
    const size_t visible_logs = 200;
    const size_t start = s.log_lines.size() > visible_logs ? (s.log_lines.size() - visible_logs) : 0;
    for (size_t i = start; i < s.log_lines.size(); ++i) {
      std::cout << "- " << clamp_visible(s.log_lines[i], kInnerWidth - 4) << '\n';
    }
    std::cout.flush();
    return;
  }

  // Keep the top dashboard fixed while letting logs append naturally below.
  save_cursor_position();
  for (size_t i = 0; i < rows.size(); ++i) {
    write_fixed_row(i + 1, rows[i]);
  }
  write_fixed_row(kLogHeaderRow, "Runtime Log (scrollable)");
  write_fixed_row(kLogSeparatorRow, std::string(kInnerWidth, '-'));
  restore_cursor_position();

  if (!log_cursor_initialized || rendered_log_lines > s.log_lines.size()) {
    std::cout << "\x1b[" << kLogStartRow << ";1H";
    rendered_log_lines = 0;
    log_cursor_initialized = true;
  }

  for (size_t i = rendered_log_lines; i < s.log_lines.size(); ++i) {
    std::cout << "\r\x1b[2K- " << clamp_visible(s.log_lines[i], kInnerWidth - 4) << '\n';
  }
  rendered_log_lines = s.log_lines.size();

  std::cout.flush();
}

} // namespace scrig
