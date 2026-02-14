#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scrig {

struct UiOptimization {
  std::string name;
  bool enabled = false;
};

struct UiSnapshot {
  uint64_t height = 0;
  double difficulty = 0.0;
  double tx_difficulty = 0.0;
  double hashrate = 0.0;
  uint64_t total_hashes = 0;
  uint64_t accepted = 0;
  uint64_t rejected = 0;
  uint32_t threads = 0;
  bool randomx = false;
  std::string node;
  std::string mode;
  std::string status;
  std::string latest_block_hash;
  uint64_t uptime_seconds = 0;
  std::vector<UiOptimization> optimizations;
  std::vector<std::string> log_lines;
};

std::string human_hashrate(double rate);
void render_dashboard(const UiSnapshot& snapshot, bool colorful);

} // namespace scrig
