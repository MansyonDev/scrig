#pragma once

#include "scrig/config.hpp"
#include "scrig/node_client.hpp"
#include "scrig/stratum_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace scrig {

class Miner {
public:
  explicit Miner(Config config);
  void request_stop();
  void request_pause();
  void request_resume();
  void report_hashrate_now();
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

  struct StratumShare {
    std::string job_id;
    std::string nonce_hex;
    std::string nonce_hex_alt;
    std::string result_hex;
    uint64_t height = 0;
  };

  MiningJob build_solo_job(NodeClient& client);
  MiningJob build_pool_job(const Block& pool_block, const DifficultyTarget& pool_difficulty);

  void mine_reward_transaction(Block& block);
  MiningResult mine_block(
    const MiningJob& job,
    std::atomic<bool>& cancel_signal,
    const std::atomic<uint64_t>* cancel_version = nullptr,
    uint64_t cancel_version_expected = 0);
  void ensure_mining_workers_started();
  void stop_mining_workers();
  void mining_worker_loop(uint32_t worker_id, uint32_t thread_count);

  bool submit_candidate(NodeClient& client, MiningJob& job, const MiningResult& result);

  void run_solo();
  void run_pool();
  void run_pool_stratum();

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
  std::atomic<bool> wallet_balance_available_{false};
  std::atomic<uint64_t> wallet_balance_nano_{0};
  std::atomic<uint64_t> wallet_baseline_nano_{0};
  std::atomic<int64_t> wallet_delta_nano_{0};
  std::atomic<bool> wallet_baseline_set_{false};

  std::atomic<bool> shutdown_ui_{false};
  std::atomic<bool> paused_{false};

  std::mutex status_mutex_;
  std::mutex log_mutex_;
  std::string status_ = "Starting";
  std::string latest_hash_;
  std::vector<std::string> runtime_logs_;
  std::atomic<uint64_t> last_logged_job_height_{UINT64_MAX};
  std::vector<Transaction> cached_mempool_;
  std::chrono::steady_clock::time_point cached_mempool_at_{};

  std::mutex work_mutex_;
  std::condition_variable work_cv_;
  std::condition_variable work_done_cv_;
  std::vector<std::thread> mining_workers_;
  MiningJob active_job_{};
  std::atomic<bool>* active_cancel_signal_ = nullptr;
  const std::atomic<uint64_t>* active_cancel_version_ = nullptr;
  uint64_t active_cancel_version_expected_ = 0;
  uint64_t work_generation_ = 0;
  uint32_t workers_completed_ = 0;
  bool work_stop_ = false;
  bool work_active_ = false;
  bool work_finished_ = false;
  std::exception_ptr work_error_;
  std::atomic<bool> work_quit_{false};
  std::atomic<bool> work_found_{false};
  std::atomic<uint64_t> work_found_nonce_{0};
  std::mutex work_found_mutex_;
  Hash work_found_hash_ = Hash::zero();

  std::chrono::steady_clock::time_point start_time_{};
};

} // namespace scrig
