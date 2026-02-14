#include "scrig/ui.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace scrig {

namespace {

constexpr size_t kInnerWidth = 108;
constexpr size_t kTopRows = 15;
constexpr size_t kLogHeaderRow = kTopRows + 1;
constexpr size_t kLogSeparatorRow = kTopRows + 2;
constexpr size_t kLogStartRow = kTopRows + 3;

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

std::string pad_right_visible(const std::string& text, size_t width) {
  const size_t vis = visible_length(text);
  if (vis >= width) {
    return text;
  }
  return text + std::string(width - vis, ' ');
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

std::string box_border() {
  return "+" + std::string(kInnerWidth, '-') + "+";
}

void write_fixed_row(size_t row, const std::string& text) {
  std::cout << "\x1b[" << row << ";1H\x1b[2K" << text;
}

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

void render_dashboard(const UiSnapshot& s, bool colorful) {
  static bool initialized = false;
  static size_t rendered_log_lines = 0;

  if (!initialized) {
    std::cout << "\x1b[2J";
    initialized = true;
  }
  std::cout << "\x1b[H";

  const std::string title = colorize(" SCRIG - Snap Coin Miner ", "\x1b[1;36m", colorful);
  const std::string status = colorize(" " + s.status + " ", "\x1b[1;32m", colorful);
  std::unordered_map<std::string, bool> opt_map;
  opt_map.reserve(s.optimizations.size());
  for (const auto& opt : s.optimizations) {
    opt_map[opt.name] = opt.enabled;
  }

  std::vector<std::string> rows;
  rows.reserve(kTopRows);
  rows.push_back(box_border());
  rows.push_back("|" + pad_right_visible(title, kInnerWidth) + "|");
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
    " Accepted: " + std::to_string(s.accepted) +
    "    Rejected: " + std::to_string(s.rejected), kInnerWidth) + "|");

  std::string hash_line = s.latest_block_hash;
  if (hash_line.size() > 64) {
    hash_line = hash_line.substr(0, 64);
  }
  rows.push_back("|" + pad_right_visible(" Last Block Hash: " + hash_line, kInnerWidth) + "|");
  rows.push_back("|" + pad_right_visible(" Status: " + status, kInnerWidth) + "|");
  rows.push_back(box_border());

  for (size_t i = 0; i < rows.size(); ++i) {
    write_fixed_row(i + 1, rows[i]);
  }
  write_fixed_row(kLogHeaderRow, "Runtime Log (scrollable)");
  write_fixed_row(kLogSeparatorRow, std::string(kInnerWidth, '-'));

  if (rendered_log_lines > s.log_lines.size()) {
    rendered_log_lines = 0;
  }
  for (size_t i = rendered_log_lines; i < s.log_lines.size(); ++i) {
    write_fixed_row(kLogStartRow + i, "- " + s.log_lines[i]);
  }
  rendered_log_lines = s.log_lines.size();

  std::cout << "\x1b[" << (kLogStartRow + rendered_log_lines) << ";1H";

  std::cout.flush();
}

} // namespace scrig
