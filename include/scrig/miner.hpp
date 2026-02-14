#pragma once

#include "scrig/config.hpp"
#include "scrig/node_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scrig {

class Miner {
public:
  explicit Miner(Config config);
  void request_stop();
  void add_runtime_note(std::string note);
  void run();

private:
  struct MiningJob {
    Block block;
    DifficultyTarget target{};
    std::vector<uint8_t> block_pow_template;
    size_t block_nonce_offset = 0;
    uint64_t base_nonce = 0x100000000ULL;
  };

  struct MiningResult {
    bool found = false;
    uint64_t nonce = 0;
    Hash hash = Hash::zero();
  };

  MiningJob build_solo_job(NodeClient& client);
  MiningJob build_pool_job(const Block& pool_block, const DifficultyTarget& pool_difficulty);

  void mine_reward_transaction(Block& block);
  MiningResult mine_block(const MiningJob& job, std::atomic<bool>& cancel_signal);

  bool submit_candidate(NodeClient& client, MiningJob& job, const MiningResult& result);

  void run_solo();
  void run_pool();

  void refresh_stats_loop();
  void set_status(const std::string& status);
  void push_log(std::string line);

  Config config_;
  PublicKey miner_public_{};

  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> total_hashes_{0};
  std::atomic<uint64_t> accepted_{0};
  std::atomic<uint64_t> rejected_{0};
  std::atomic<double> current_hashrate_{0.0};
  std::atomic<double> current_difficulty_{0.0};
  std::atomic<double> current_tx_difficulty_{0.0};
  std::atomic<uint64_t> current_height_{0};

  std::atomic<bool> shutdown_ui_{false};

  std::mutex status_mutex_;
  std::mutex log_mutex_;
  std::string status_ = "Starting";
  std::string latest_hash_;
  std::vector<std::string> runtime_logs_;
  std::atomic<uint64_t> last_logged_job_height_{UINT64_MAX};
  std::vector<Transaction> cached_mempool_;
  std::chrono::steady_clock::time_point cached_mempool_at_{};

  std::chrono::steady_clock::time_point start_time_{};
};

} // namespace scrig
