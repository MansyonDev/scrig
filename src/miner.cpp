#include "scrig/miner.hpp"

#include "scrig/consensus.hpp"
#include "scrig/perf.hpp"
#include "scrig/ui.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace scrig {

namespace {

double target_to_display_difficulty(const DifficultyTarget& target) {
  uint64_t prefix = 0;
  for (size_t i = 0; i < 8; ++i) {
    prefix = (prefix << 8U) | static_cast<uint64_t>(target[i]);
  }

  if (prefix == 0) {
    return 0.0;
  }

  return static_cast<double>(std::numeric_limits<uint64_t>::max()) / static_cast<double>(prefix);
}

std::vector<Transaction> filter_mempool(const std::vector<Transaction>& mempool) {
  std::vector<Transaction> out;
  out.reserve(std::min<size_t>(mempool.size(), static_cast<size_t>(SNAP_MAX_TRANSACTIONS_PER_BLOCK - 1)));

  const auto now_ts = unix_timestamp_now();
  for (const auto& tx : mempool) {
    if (tx.timestamp + 5ULL >= SNAP_EXPIRATION_TIME_SECONDS + now_ts) {
      continue;
    }

    out.push_back(tx);
    if (out.size() >= static_cast<size_t>(SNAP_MAX_TRANSACTIONS_PER_BLOCK - 1)) {
      break;
    }
  }

  return out;
}

constexpr uint64_t kHashFlushBatch = 4096;
constexpr std::chrono::milliseconds kHashFlushInterval{1500};
constexpr uint32_t kHotLoopControlCheckInterval = 8;
constexpr uint32_t kRandomXPipelineBatchMin = 4;
constexpr uint32_t kRandomXPipelineBatchMax = 32;
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
constexpr uint32_t kRandomXPipelineBatchDefault = 16;
#else
constexpr uint32_t kRandomXPipelineBatchDefault = 8;
#endif
constexpr size_t kMaxRuntimeLogLines = 5000;

std::atomic<uint64_t> g_nonce_seed{0x9E3779B97F4A7C15ULL};
std::atomic<uint32_t> g_pipeline_batch{kRandomXPipelineBatchDefault};

uint64_t next_nonce_base() {
  // SplitMix64-style generator, cheap and deterministic enough for nonce spacing.
  uint64_t x = g_nonce_seed.fetch_add(0x9E3779B97F4A7C15ULL, std::memory_order_relaxed);
  x ^= x >> 30U;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27U;
  x *= 0x94D049BB133111EBULL;
  x ^= x >> 31U;
  return 0x100000000ULL | (x & 0xFFFFFFFFULL);
}

inline void write_nonce_le(uint8_t* dest, uint64_t nonce) {
#if defined(__LITTLE_ENDIAN__) || \
  (defined(_MSC_VER) && !defined(__clang__)) || \
  (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
  std::memcpy(dest, &nonce, sizeof(nonce));
#else
  for (size_t i = 0; i < sizeof(nonce); ++i) {
    dest[i] = static_cast<uint8_t>((nonce >> (8U * i)) & 0xFFU);
  }
#endif
}

uint32_t clamp_pipeline_batch(uint32_t batch) {
  if (batch == 0) {
    return kRandomXPipelineBatchDefault;
  }
  return std::clamp(batch, kRandomXPipelineBatchMin, kRandomXPipelineBatchMax);
}

uint32_t current_pipeline_batch() {
  return clamp_pipeline_batch(g_pipeline_batch.load(std::memory_order_relaxed));
}

void set_pipeline_batch(uint32_t batch) {
  g_pipeline_batch.store(clamp_pipeline_batch(batch), std::memory_order_relaxed);
}

} // namespace

Miner::Miner(Config config)
  : config_(std::move(config)),
    miner_public_(public_key_from_base36(config_.wallet_address)) {}

void Miner::request_stop() {
  stop_.store(true, std::memory_order_relaxed);
  work_quit_.store(true, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    work_stop_ = true;
  }
  work_cv_.notify_all();
  work_done_cv_.notify_all();
}

void Miner::request_pause() {
  const bool was_paused = paused_.exchange(true, std::memory_order_relaxed);
  set_status("Paused");
  if (!was_paused) {
    push_log("Mining paused");
  }
}

void Miner::request_resume() {
  const bool was_paused = paused_.exchange(false, std::memory_order_relaxed);
  set_status("Mining");
  if (was_paused) {
    push_log("Mining resumed");
  }
}

void Miner::report_hashrate_now() {
  push_log(
    "Hashrate: " + human_hashrate(current_hashrate_.load(std::memory_order_relaxed)) +
    " | height=" + std::to_string(current_height_.load(std::memory_order_relaxed)) +
    " | accepted=" + std::to_string(accepted_.load(std::memory_order_relaxed)) +
    " | rejected=" + std::to_string(rejected_.load(std::memory_order_relaxed)));
}

void Miner::add_runtime_note(std::string note) {
  push_log(std::move(note));
}

void Miner::push_log(std::string line) {
  std::lock_guard<std::mutex> lock(log_mutex_);
  if (runtime_logs_.size() >= kMaxRuntimeLogLines) {
    const size_t drop = runtime_logs_.size() - kMaxRuntimeLogLines + 1;
    runtime_logs_.erase(
      runtime_logs_.begin(),
      runtime_logs_.begin() + static_cast<std::vector<std::string>::difference_type>(drop));
  }
  runtime_logs_.push_back(std::move(line));
}

void Miner::set_status(const std::string& status) {
  std::lock_guard<std::mutex> lock(status_mutex_);
  status_ = status;
}

void Miner::ensure_mining_workers_started() {
  if (!mining_workers_.empty()) {
    return;
  }

  const uint32_t thread_count = std::max<uint32_t>(1U, config_.threads);
  work_quit_.store(false, std::memory_order_relaxed);
  work_found_.store(false, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    work_stop_ = false;
    work_active_ = false;
    work_finished_ = false;
    workers_completed_ = 0;
    work_error_ = nullptr;
    active_cancel_signal_ = nullptr;
    active_cancel_version_ = nullptr;
    active_cancel_version_expected_ = 0;
  }

  try {
    mining_workers_.reserve(thread_count);
    for (uint32_t worker_id = 0; worker_id < thread_count; ++worker_id) {
      mining_workers_.emplace_back([this, worker_id, thread_count]() {
        mining_worker_loop(worker_id, thread_count);
      });
    }
  } catch (...) {
    stop_mining_workers();
    throw;
  }
}

void Miner::stop_mining_workers() {
  if (mining_workers_.empty()) {
    return;
  }

  work_quit_.store(true, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    work_stop_ = true;
    work_active_ = false;
    work_finished_ = true;
    active_cancel_signal_ = nullptr;
    active_cancel_version_ = nullptr;
    active_cancel_version_expected_ = 0;
  }

  work_cv_.notify_all();
  work_done_cv_.notify_all();

  for (auto& worker : mining_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  mining_workers_.clear();
}

void Miner::mining_worker_loop(uint32_t worker_id, uint32_t thread_count) {
  (void)apply_mining_thread_priority(worker_id, thread_count, config_.performance_cores_only);
  if (config_.pin_threads) {
    (void)pin_current_thread(worker_id, thread_count, config_.performance_cores_only);
  }
  if (config_.numa_bind) {
    (void)bind_current_thread_numa(worker_id, thread_count);
  }

  uint64_t observed_generation = 0;
  while (true) {
    MiningJob job;
    std::atomic<bool>* cancel_signal = nullptr;
    const std::atomic<uint64_t>* cancel_version = nullptr;
    uint64_t cancel_version_expected = 0;
    uint64_t work_generation = 0;

    {
      std::unique_lock<std::mutex> lock(work_mutex_);
      work_cv_.wait(lock, [&]() {
        return work_stop_ || work_generation_ != observed_generation;
      });
      if (work_stop_) {
        return;
      }

      observed_generation = work_generation_;
      work_generation = work_generation_;
      job = active_job_;
      cancel_signal = active_cancel_signal_;
      cancel_version = active_cancel_version_;
      cancel_version_expected = active_cancel_version_expected_;
    }

    uint64_t local_hashes = 0;
    auto last_flush = std::chrono::steady_clock::now();

    try {
      auto local_buffer = job.block_pow_template;
      uint64_t nonce = job.base_nonce + worker_id;
      const bool fast_nonce_patch = patch_nonce_in_pow_buffer(local_buffer, job.block_nonce_offset, nonce);
      const bool use_pipeline = fast_nonce_patch && hashing_supports_pipeline();
      auto pipeline_buffer = use_pipeline ? local_buffer : std::vector<uint8_t>{};
      uint8_t* nonce_ptr = fast_nonce_patch ? local_buffer.data() + job.block_nonce_offset + 1 : nullptr;
      uint8_t* pipeline_nonce_ptr = use_pipeline ? (pipeline_buffer.data() + job.block_nonce_offset + 1) : nullptr;
      Block slow_candidate = job.block;
      slow_candidate.meta.hash.reset();

      uint32_t control_check_counter = kHotLoopControlCheckInterval;
      while (true) {
        if (++control_check_counter >= kHotLoopControlCheckInterval) {
          control_check_counter = 0;
          if (stop_.load(std::memory_order_relaxed) ||
              work_quit_.load(std::memory_order_relaxed) ||
              work_found_.load(std::memory_order_relaxed) ||
              (cancel_signal != nullptr && cancel_signal->load(std::memory_order_relaxed)) ||
              (cancel_version != nullptr &&
               cancel_version->load(std::memory_order_relaxed) != cancel_version_expected)) {
            break;
          }
          if (paused_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
          }
        }

        Hash hash{};

        if (use_pipeline) {
          const uint32_t pipeline_batch = current_pipeline_batch();
          std::array<uint64_t, kRandomXPipelineBatchMax> batch_nonces{};
          uint32_t batch_count = 0;
          for (; batch_count < pipeline_batch; ++batch_count) {
            batch_nonces[batch_count] = nonce;
            nonce += thread_count;
          }

          write_nonce_le(nonce_ptr, batch_nonces[0]);
          hash_data_pipeline_begin(local_buffer);

          bool found_in_batch = false;
          uint64_t found_nonce = 0;
          Hash found_hash = Hash::zero();

          for (uint32_t i = 1; i < batch_count; ++i) {
            uint8_t* next_nonce_ptr = (i & 1U) == 0U ? nonce_ptr : pipeline_nonce_ptr;
            auto& next_buffer = (i & 1U) == 0U ? local_buffer : pipeline_buffer;
            write_nonce_le(next_nonce_ptr, batch_nonces[i]);

            const Hash out = hash_data_pipeline_next(next_buffer);
            ++local_hashes;
            if (hash_meets_target(out, job.target)) {
              found_in_batch = true;
              found_nonce = batch_nonces[i - 1];
              found_hash = out;
              break;
            }
          }

          const Hash last_out = hash_data_pipeline_last();
          ++local_hashes;
          if (!found_in_batch && hash_meets_target(last_out, job.target)) {
            found_in_batch = true;
            found_nonce = batch_nonces[batch_count - 1];
            found_hash = last_out;
          }

          if (found_in_batch) {
            bool expected = false;
            if (work_found_.compare_exchange_strong(
                  expected,
                  true,
                  std::memory_order_acq_rel,
                  std::memory_order_relaxed)) {
              work_found_nonce_.store(found_nonce, std::memory_order_relaxed);
              std::lock_guard<std::mutex> found_lock(work_found_mutex_);
              work_found_hash_ = found_hash;
            }
            break;
          }
        } else if (fast_nonce_patch) {
          write_nonce_le(nonce_ptr, nonce);
          hash = hash_data(local_buffer);
          nonce += thread_count;
          ++local_hashes;

          if (hash_meets_target(hash, job.target)) {
            bool expected = false;
            if (work_found_.compare_exchange_strong(
                  expected,
                  true,
                  std::memory_order_acq_rel,
                  std::memory_order_relaxed)) {
              work_found_nonce_.store(nonce - thread_count, std::memory_order_relaxed);
              std::lock_guard<std::mutex> found_lock(work_found_mutex_);
              work_found_hash_ = hash;
            }
            break;
          }
        } else {
          slow_candidate.nonce = nonce;
          hash = compute_block_hash(slow_candidate);
          ++local_hashes;
          if (hash_meets_target(hash, job.target)) {
            bool expected = false;
            if (work_found_.compare_exchange_strong(
                  expected,
                  true,
                  std::memory_order_acq_rel,
                  std::memory_order_relaxed)) {
              work_found_nonce_.store(nonce, std::memory_order_relaxed);
              std::lock_guard<std::mutex> found_lock(work_found_mutex_);
              work_found_hash_ = hash;
            }
            break;
          }
          nonce += thread_count;
        }

        const auto now = std::chrono::steady_clock::now();
        if (local_hashes >= kHashFlushBatch || (now - last_flush) >= kHashFlushInterval) {
          total_hashes_.fetch_add(local_hashes, std::memory_order_relaxed);
          local_hashes = 0;
          last_flush = now;
        }
      }
    } catch (...) {
      work_found_.store(true, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(work_mutex_);
      if (work_generation == work_generation_ && work_error_ == nullptr) {
        work_error_ = std::current_exception();
      }
    }

    if (local_hashes > 0) {
      total_hashes_.fetch_add(local_hashes, std::memory_order_relaxed);
    }

    {
      std::lock_guard<std::mutex> lock(work_mutex_);
      if (work_generation == work_generation_) {
        ++workers_completed_;
        if (workers_completed_ >= thread_count) {
          work_active_ = false;
          work_finished_ = true;
          work_done_cv_.notify_one();
        }
      }
    }
  }
}

Miner::MiningJob Miner::build_solo_job(NodeClient& client) {
  const auto height = client.height();
  const auto diffs = client.difficulty();
  const auto reward = client.reward();

  current_height_.store(height, std::memory_order_relaxed);
  current_difficulty_.store(target_to_display_difficulty(diffs.block_difficulty), std::memory_order_relaxed);
  current_tx_difficulty_.store(target_to_display_difficulty(diffs.transaction_difficulty), std::memory_order_relaxed);

  std::vector<Transaction> txs;
  if (config_.include_mempool_transactions) {
    const auto now = std::chrono::steady_clock::now();
    if (cached_mempool_.empty() ||
        std::chrono::duration_cast<std::chrono::seconds>(now - cached_mempool_at_).count() >= 2) {
      try {
        cached_mempool_ = filter_mempool(client.mempool_all());
        cached_mempool_at_ = now;
      } catch (...) {
        // Keep previous cache if mempool refresh fails.
      }
    }
    txs = cached_mempool_;
  }

  Transaction reward_tx = build_reward_transaction(miner_public_, reward, unix_timestamp_now());
  txs.push_back(std::move(reward_tx));

  Block block;
  block.transactions = std::move(txs);
  block.timestamp = unix_timestamp_now();
  block.nonce = 0x100000000ULL;

  block.meta.block_pow_difficulty = diffs.block_difficulty;
  block.meta.tx_pow_difficulty = diffs.transaction_difficulty;

  if (height == 0) {
    block.meta.previous_block = genesis_previous_block_hash();
  } else {
    const auto prev = client.block_hash(height - 1);
    block.meta.previous_block = prev.value_or(genesis_previous_block_hash());
  }

  block.meta.hash.reset();
  mine_reward_transaction(block);

  const auto prev_height = last_logged_job_height_.load(std::memory_order_relaxed);
  if (prev_height != height) {
    last_logged_job_height_.store(height, std::memory_order_relaxed);
    push_log(
      "Solo job ready at height " + std::to_string(height) +
      " with " + std::to_string(block.transactions.size()) + " tx(s)");
  }

  MiningJob job;
  job.block = std::move(block);
  job.target = calculate_block_difficulty_target(job.block.meta.block_pow_difficulty, job.block.transactions.size());
  job.block_pow_template = block_pow_buffer(job.block, &job.block_nonce_offset);
  job.base_nonce = next_nonce_base();
  return job;
}

Miner::MiningJob Miner::build_pool_job(const Block& pool_block, const DifficultyTarget& pool_difficulty) {
  Block block = pool_block;
  block.meta.hash.reset();
  block.nonce = 0x100000000ULL;

  trim_expired_transactions(block, unix_timestamp_now());

  MiningJob job;
  job.block = std::move(block);
  job.target = pool_difficulty;
  job.block_pow_template = block_pow_buffer(job.block, &job.block_nonce_offset);
  job.base_nonce = next_nonce_base();

  current_difficulty_.store(target_to_display_difficulty(pool_difficulty), std::memory_order_relaxed);
  return job;
}

void Miner::mine_reward_transaction(Block& block) {
  if (block.transactions.empty()) {
    throw std::runtime_error("cannot mine reward transaction in empty block");
  }

  auto& reward_tx = block.transactions.back();
  reward_tx.transaction_id.reset();
  reward_tx.nonce = 0x100000000ULL;

  size_t nonce_offset = 0;
  auto pow_template = transaction_pow_buffer(reward_tx, &nonce_offset);
  std::atomic<bool> found{false};
  std::atomic<uint64_t> found_nonce{0};
  Hash found_hash = Hash::zero();
  std::mutex found_lock;
  std::mutex error_lock;
  std::exception_ptr worker_error = nullptr;

  const uint32_t thread_count = std::max<uint32_t>(1U, config_.threads);
  std::vector<std::thread> workers;
  workers.reserve(thread_count);

  for (uint32_t worker_id = 0; worker_id < thread_count; ++worker_id) {
    workers.emplace_back([&, worker_id]() {
      try {
        (void)apply_mining_thread_priority(worker_id, thread_count, config_.performance_cores_only);
        if (config_.pin_threads) {
          (void)pin_current_thread(worker_id, thread_count, config_.performance_cores_only);
        }
        if (config_.numa_bind) {
          (void)bind_current_thread_numa(worker_id, thread_count);
        }

        auto local_buffer = pow_template;
        uint64_t nonce = reward_tx.nonce + worker_id;
        uint64_t local_hashes = 0;
        auto last_flush = std::chrono::steady_clock::now();
        const bool fast_nonce_patch = patch_nonce_in_pow_buffer(local_buffer, nonce_offset, nonce);
        const bool use_pipeline = fast_nonce_patch && hashing_supports_pipeline();
        auto pipeline_buffer = use_pipeline ? local_buffer : std::vector<uint8_t>{};
        uint8_t* nonce_ptr = fast_nonce_patch ? local_buffer.data() + nonce_offset + 1 : nullptr;
        uint8_t* pipeline_nonce_ptr = use_pipeline ? (pipeline_buffer.data() + nonce_offset + 1) : nullptr;
        Transaction slow_candidate = reward_tx;

        uint32_t control_check_counter = kHotLoopControlCheckInterval;
        while (true) {
          if (++control_check_counter >= kHotLoopControlCheckInterval) {
            control_check_counter = 0;
            if (stop_.load(std::memory_order_relaxed) || found.load(std::memory_order_relaxed)) {
              break;
            }
            if (paused_.load(std::memory_order_relaxed)) {
              std::this_thread::sleep_for(std::chrono::milliseconds(50));
              continue;
            }
          }

          Hash hash{};

          if (use_pipeline) {
            const uint32_t pipeline_batch = current_pipeline_batch();
            std::array<uint64_t, kRandomXPipelineBatchMax> batch_nonces{};
            uint32_t batch_count = 0;
            for (; batch_count < pipeline_batch; ++batch_count) {
              batch_nonces[batch_count] = nonce;
              nonce += thread_count;
            }

            write_nonce_le(nonce_ptr, batch_nonces[0]);
            hash_data_pipeline_begin(local_buffer);

            bool found_in_batch = false;
            uint64_t found_nonce_local = 0;
            Hash found_hash_local = Hash::zero();

            for (uint32_t i = 1; i < batch_count; ++i) {
              uint8_t* next_nonce_ptr = (i & 1U) == 0U ? nonce_ptr : pipeline_nonce_ptr;
              auto& next_buffer = (i & 1U) == 0U ? local_buffer : pipeline_buffer;
              write_nonce_le(next_nonce_ptr, batch_nonces[i]);

              const Hash out = hash_data_pipeline_next(next_buffer);
              ++local_hashes;
              if (hash_meets_target(out, block.meta.tx_pow_difficulty)) {
                found_in_batch = true;
                found_nonce_local = batch_nonces[i - 1];
                found_hash_local = out;
                break;
              }
            }

            const Hash last_out = hash_data_pipeline_last();
            ++local_hashes;
            if (!found_in_batch && hash_meets_target(last_out, block.meta.tx_pow_difficulty)) {
              found_in_batch = true;
              found_nonce_local = batch_nonces[batch_count - 1];
              found_hash_local = last_out;
            }

            if (found_in_batch) {
              bool expected = false;
              if (found.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                found_nonce.store(found_nonce_local, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(found_lock);
                found_hash = found_hash_local;
              }
              break;
            }
          } else if (fast_nonce_patch) {
            write_nonce_le(nonce_ptr, nonce);
            hash = hash_data(local_buffer);
            nonce += thread_count;
            ++local_hashes;

            if (hash_meets_target(hash, block.meta.tx_pow_difficulty)) {
              bool expected = false;
              if (found.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                found_nonce.store(nonce - thread_count, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(found_lock);
                found_hash = hash;
              }
              break;
            }
          } else {
            slow_candidate.nonce = nonce;
            hash = compute_transaction_hash(slow_candidate);
            ++local_hashes;
            if (hash_meets_target(hash, block.meta.tx_pow_difficulty)) {
              bool expected = false;
              if (found.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                found_nonce.store(nonce, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(found_lock);
                found_hash = hash;
              }
              break;
            }
            nonce += thread_count;
          }

          const auto now = std::chrono::steady_clock::now();
          if (local_hashes >= kHashFlushBatch || (now - last_flush) >= kHashFlushInterval) {
            total_hashes_.fetch_add(local_hashes, std::memory_order_relaxed);
            local_hashes = 0;
            last_flush = now;
          }
        }

        if (local_hashes > 0) {
          total_hashes_.fetch_add(local_hashes, std::memory_order_relaxed);
        }
      } catch (...) {
        found.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(error_lock);
        if (worker_error == nullptr) {
          worker_error = std::current_exception();
        }
      }
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  if (worker_error != nullptr) {
    std::rethrow_exception(worker_error);
  }

  if (!found.load(std::memory_order_relaxed)) {
    throw std::runtime_error("reward transaction mining interrupted");
  }

  reward_tx.nonce = found_nonce.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(found_lock);
    reward_tx.transaction_id = found_hash;
  }
  block.meta.merkle_tree_root = build_merkle_root(block.transactions);
  block.meta.address_inclusion_filter = build_address_filter(block.transactions);
}

Miner::MiningResult Miner::mine_block(
  const MiningJob& job,
  std::atomic<bool>& cancel_signal,
  const std::atomic<uint64_t>* cancel_version,
  uint64_t cancel_version_expected) {
  ensure_mining_workers_started();

  MiningResult result;
  auto clear_cancel_refs = [&]() {
    std::lock_guard<std::mutex> lock(work_mutex_);
    active_cancel_signal_ = nullptr;
    active_cancel_version_ = nullptr;
    active_cancel_version_expected_ = 0;
  };

  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    active_job_ = job;
    active_cancel_signal_ = &cancel_signal;
    active_cancel_version_ = cancel_version;
    active_cancel_version_expected_ = cancel_version_expected;
    workers_completed_ = 0;
    work_active_ = true;
    work_finished_ = false;
    work_error_ = nullptr;
    work_found_.store(false, std::memory_order_relaxed);
    work_found_nonce_.store(0, std::memory_order_relaxed);
    work_found_hash_ = Hash::zero();
    ++work_generation_;
  }

  work_cv_.notify_all();

  {
    std::unique_lock<std::mutex> lock(work_mutex_);
    work_done_cv_.wait(lock, [&]() {
      return work_finished_ || stop_.load(std::memory_order_relaxed);
    });
    if (!work_finished_) {
      lock.unlock();
      clear_cancel_refs();
      return result;
    }
    if (work_error_ != nullptr) {
      const auto error = work_error_;
      lock.unlock();
      clear_cancel_refs();
      std::rethrow_exception(error);
    }
  }

  if (work_found_.load(std::memory_order_relaxed)) {
    result.found = true;
    result.nonce = work_found_nonce_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> found_lock(work_found_mutex_);
    result.hash = work_found_hash_;
  }

  clear_cancel_refs();

  return result;
}

bool Miner::submit_candidate(NodeClient& client, MiningJob& job, const MiningResult& result) {
  job.block.nonce = result.nonce;
  job.block.meta.hash = result.hash;

  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    latest_hash_ = hash_to_base36(result.hash);
    status_ = "Submitting";
  }

  const bool accepted = client.submit_block(job.block);
  if (accepted) {
    accepted_.fetch_add(1, std::memory_order_relaxed);
    set_status("Accepted");
  } else {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    set_status("Rejected");
  }

  return accepted;
}

double Miner::benchmark_hashing_throughput(
  uint32_t threads,
  uint32_t pipeline_batch,
  std::chrono::seconds duration) {
  if (duration.count() <= 0) {
    return 0.0;
  }

  const uint32_t thread_count = std::max<uint32_t>(1U, threads);
  const uint32_t bench_batch = clamp_pipeline_batch(pipeline_batch);
  const bool use_pipeline = hashing_supports_pipeline();

  // Minimal stable payload with a fixed-width nonce varint at a known offset.
  std::vector<uint8_t> base_buffer(160, 0x42U);
  const size_t nonce_offset = 24;
  base_buffer[nonce_offset] = 253U;
  for (size_t i = 0; i < 8; ++i) {
    base_buffer[nonce_offset + 1 + i] = 0U;
  }

  std::atomic<uint64_t> total_hashes{0};
  std::atomic<bool> stop_now{false};
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + duration;

  std::vector<std::thread> workers;
  workers.reserve(thread_count);

  for (uint32_t worker_id = 0; worker_id < thread_count; ++worker_id) {
    workers.emplace_back([&, worker_id]() {
      (void)apply_mining_thread_priority(worker_id, thread_count, config_.performance_cores_only);
      if (config_.pin_threads) {
        (void)pin_current_thread(worker_id, thread_count, config_.performance_cores_only);
      }
      if (config_.numa_bind) {
        (void)bind_current_thread_numa(worker_id, thread_count);
      }

      auto local_buffer = base_buffer;
      uint8_t* nonce_ptr = local_buffer.data() + nonce_offset + 1;
      auto pipeline_buffer = use_pipeline ? local_buffer : std::vector<uint8_t>{};
      uint8_t* pipeline_nonce_ptr = use_pipeline ? (pipeline_buffer.data() + nonce_offset + 1) : nullptr;

      uint64_t nonce = next_nonce_base() + worker_id;
      uint64_t local_hashes = 0;
      uint32_t control_check_counter = kHotLoopControlCheckInterval;

      while (true) {
        if (++control_check_counter >= kHotLoopControlCheckInterval) {
          control_check_counter = 0;
          if (stop_.load(std::memory_order_relaxed) || stop_now.load(std::memory_order_relaxed)) {
            break;
          }
          if (std::chrono::steady_clock::now() >= deadline) {
            stop_now.store(true, std::memory_order_relaxed);
            break;
          }
        }

        if (use_pipeline) {
          std::array<uint64_t, kRandomXPipelineBatchMax> batch_nonces{};
          uint32_t batch_count = 0;
          for (; batch_count < bench_batch; ++batch_count) {
            batch_nonces[batch_count] = nonce;
            nonce += thread_count;
          }

          write_nonce_le(nonce_ptr, batch_nonces[0]);
          hash_data_pipeline_begin(local_buffer);
          for (uint32_t i = 1; i < batch_count; ++i) {
            uint8_t* next_nonce_ptr = (i & 1U) == 0U ? nonce_ptr : pipeline_nonce_ptr;
            auto& next_buffer = (i & 1U) == 0U ? local_buffer : pipeline_buffer;
            write_nonce_le(next_nonce_ptr, batch_nonces[i]);
            (void)hash_data_pipeline_next(next_buffer);
            ++local_hashes;
          }
          (void)hash_data_pipeline_last();
          ++local_hashes;
        } else {
          write_nonce_le(nonce_ptr, nonce);
          nonce += thread_count;
          (void)hash_data(local_buffer);
          ++local_hashes;
        }
      }

      total_hashes.fetch_add(local_hashes, std::memory_order_relaxed);
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (elapsed <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(total_hashes.load(std::memory_order_relaxed)) / elapsed;
}

void Miner::maybe_auto_tune_startup() {
  set_pipeline_batch(config_.randomx_pipeline_batch);

  if (!config_.auto_tune_startup || stop_.load(std::memory_order_relaxed)) {
    return;
  }

  const uint32_t logical = logical_cpu_count();
  const uint32_t physical = physical_cpu_count();
  const uint32_t perf_logical = performance_core_logical_count();
  const uint32_t perf_physical = performance_core_physical_count();
  const uint32_t current_threads = std::max<uint32_t>(1U, config_.threads);
  const uint32_t recommended_threads = std::max<uint32_t>(
    1U,
    recommended_mining_threads(config_.performance_cores_only));
  const uint32_t requested_total_seconds = std::clamp<uint32_t>(config_.auto_tune_seconds, 6U, 120U);

  std::set<uint32_t> thread_candidates_set;
  const auto push_thread_candidate = [&](uint32_t candidate) {
    if (candidate == 0) {
      return;
    }
    const uint32_t bounded = std::clamp<uint32_t>(candidate, 1U, std::max<uint32_t>(1U, logical));
    thread_candidates_set.insert(bounded);
  };

  push_thread_candidate(current_threads);
  push_thread_candidate(recommended_threads);
  push_thread_candidate(recommended_threads > 2U ? (recommended_threads - 2U) : 1U);
  push_thread_candidate(recommended_threads > 1U ? (recommended_threads - 1U) : 1U);
  push_thread_candidate(recommended_threads + 1U);
  push_thread_candidate(recommended_threads + 2U);
  push_thread_candidate(physical);
  push_thread_candidate(logical);
  push_thread_candidate(logical > 3U ? (logical - 2U) : logical);
  push_thread_candidate(logical >= 4U ? (logical / 2U) : logical);

  if (hybrid_topology_detected()) {
    push_thread_candidate(perf_physical);
    push_thread_candidate(perf_logical);
  }

  std::vector<uint32_t> thread_candidates(thread_candidates_set.begin(), thread_candidates_set.end());

  std::vector<uint32_t> batch_candidates{current_pipeline_batch(), 8U, 16U, 24U, 32U};
  for (auto& b : batch_candidates) {
    b = clamp_pipeline_batch(b);
  }
  std::sort(batch_candidates.begin(), batch_candidates.end());
  batch_candidates.erase(std::unique(batch_candidates.begin(), batch_candidates.end()), batch_candidates.end());

  const uint32_t bench_cases =
    static_cast<uint32_t>(thread_candidates.size() + (hashing_supports_pipeline() ? batch_candidates.size() : 0U));
  const uint32_t per_case_seconds =
    std::max<uint32_t>(2U, requested_total_seconds / std::max<uint32_t>(1U, bench_cases));
  const auto bench_duration = std::chrono::seconds(per_case_seconds);

  push_log(
    "Auto-tune: benchmarking " + std::to_string(bench_cases) +
    " case(s), " + std::to_string(per_case_seconds) + "s each");

  uint32_t best_threads = current_threads;
  uint32_t best_batch = current_pipeline_batch();
  double best_rate = benchmark_hashing_throughput(best_threads, best_batch, bench_duration);
  push_log(
    "Auto-tune: baseline threads=" + std::to_string(best_threads) +
    " pipeline_batch=" + std::to_string(best_batch) +
    " rate=" + human_hashrate(best_rate));

  for (const auto candidate_threads : thread_candidates) {
    if (stop_.load(std::memory_order_relaxed)) {
      return;
    }
    const double rate = benchmark_hashing_throughput(candidate_threads, best_batch, bench_duration);
    push_log(
      "Auto-tune: threads=" + std::to_string(candidate_threads) +
      " rate=" + human_hashrate(rate));
    if (rate > best_rate) {
      best_rate = rate;
      best_threads = candidate_threads;
    }
  }

  if (hashing_supports_pipeline()) {
    for (const auto candidate_batch : batch_candidates) {
      if (stop_.load(std::memory_order_relaxed)) {
        return;
      }
      const double rate = benchmark_hashing_throughput(best_threads, candidate_batch, bench_duration);
      push_log(
        "Auto-tune: pipeline_batch=" + std::to_string(candidate_batch) +
        " rate=" + human_hashrate(rate));
      if (rate > best_rate) {
        best_rate = rate;
        best_batch = candidate_batch;
      }
    }
  }

  config_.threads = best_threads;
  config_.randomx_pipeline_batch = best_batch;
  set_pipeline_batch(best_batch);
  push_log(
    "Auto-tune result: threads=" + std::to_string(best_threads) +
    " pipeline_batch=" + std::to_string(best_batch) +
    " tuned_rate=" + human_hashrate(best_rate));
}

void Miner::refresh_stats_loop() {
  uint64_t last_hashes = total_hashes_.load(std::memory_order_relaxed);
  auto last_tick = std::chrono::steady_clock::now();
  uint64_t last_summary_uptime = 0;
  std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> hashrate_samples;
  constexpr auto kHashrateWindow = std::chrono::seconds(8);
  constexpr auto kHashrateWindowMaxAge = std::chrono::seconds(12);

  while (!shutdown_ui_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::seconds(1));

    const auto now = std::chrono::steady_clock::now();
    const auto current_hashes = total_hashes_.load(std::memory_order_relaxed);
    const auto elapsed = std::chrono::duration<double>(now - last_tick).count();

    if (elapsed > 0.0) {
      const auto delta = static_cast<double>(current_hashes - last_hashes);
      const double instant_rate = delta / elapsed;
      hashrate_samples.emplace_back(now, current_hashes);
      while (!hashrate_samples.empty() && (now - hashrate_samples.front().first) > kHashrateWindowMaxAge) {
        hashrate_samples.pop_front();
      }

      double window_rate = instant_rate;
      if (!hashrate_samples.empty()) {
        auto sample = hashrate_samples.front();
        for (auto it = hashrate_samples.rbegin(); it != hashrate_samples.rend(); ++it) {
          if ((now - it->first) >= kHashrateWindow) {
            sample = *it;
            break;
          }
        }

        const auto window_seconds = std::chrono::duration<double>(now - sample.first).count();
        if (window_seconds > 0.5) {
          const auto window_delta = static_cast<double>(current_hashes - sample.second);
          window_rate = window_delta / window_seconds;
        }
      }

      const double prev_rate = current_hashrate_.load(std::memory_order_relaxed);
      const double smoothed_rate = (prev_rate <= 0.0)
        ? window_rate
        : (prev_rate * 0.85 + window_rate * 0.15);
      current_hashrate_.store(smoothed_rate, std::memory_order_relaxed);
    }

    last_hashes = current_hashes;
    last_tick = now;

    const auto uptime_seconds = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
    if (uptime_seconds >= last_summary_uptime + 10ULL) {
      last_summary_uptime = uptime_seconds;
      push_log(
        "Stats: hashrate=" + human_hashrate(current_hashrate_.load(std::memory_order_relaxed)) +
        " | height=" + std::to_string(current_height_.load(std::memory_order_relaxed)) +
        " | accepted=" + std::to_string(accepted_.load(std::memory_order_relaxed)) +
        " | rejected=" + std::to_string(rejected_.load(std::memory_order_relaxed)));
    }

    if (config_.dashboard) {
      UiSnapshot snapshot;
      snapshot.height = current_height_.load(std::memory_order_relaxed);
      snapshot.difficulty = current_difficulty_.load(std::memory_order_relaxed);
      snapshot.tx_difficulty = current_tx_difficulty_.load(std::memory_order_relaxed);
      snapshot.hashrate = current_hashrate_.load(std::memory_order_relaxed);
      snapshot.total_hashes = total_hashes_.load(std::memory_order_relaxed);
      snapshot.accepted = accepted_.load(std::memory_order_relaxed);
      snapshot.rejected = rejected_.load(std::memory_order_relaxed);
      snapshot.wallet_balance_available = wallet_balance_available_.load(std::memory_order_relaxed);
      snapshot.wallet_balance_nano = wallet_balance_nano_.load(std::memory_order_relaxed);
      snapshot.wallet_delta_nano = wallet_delta_nano_.load(std::memory_order_relaxed);
      snapshot.threads = config_.threads;
      snapshot.randomx = hashing_uses_randomx();
      snapshot.node = config_.mode == "pool"
        ? (config_.pool_host + ":" + std::to_string(config_.pool_port))
        : (config_.node_host + ":" + std::to_string(config_.node_port));
      snapshot.mode = config_.mode;
      snapshot.uptime_seconds = uptime_seconds;
      snapshot.optimizations = {
        {"pin_threads", config_.pin_threads},
        {"numa_bind", config_.numa_bind},
        {"chain_events", config_.use_chain_events},
        {"rx_jit", config_.randomx_jit},
        {"rx_full_mem", config_.randomx_full_mem},
        {"rx_huge_pages", config_.randomx_huge_pages},
        {"rx_hard_aes", config_.randomx_hard_aes},
        {"rx_secure", config_.randomx_secure},
      };

      {
        std::lock_guard<std::mutex> lock(status_mutex_);
        snapshot.status = status_;
        snapshot.latest_block_hash = latest_hash_;
      }
      {
        std::lock_guard<std::mutex> lock(log_mutex_);
        snapshot.log_lines = runtime_logs_;
      }

      render_dashboard(snapshot, config_.colorful_ui);
    }
  }
}

void Miner::run_solo() {
  NodeClient client(config_.node_host, config_.node_port);
  client.connect();
  push_log("Connected to node " + config_.node_host + ":" + std::to_string(config_.node_port));
  try {
    const auto height = client.height();
    current_height_.store(height, std::memory_order_relaxed);
    push_log("Node height: " + std::to_string(height));
  } catch (...) {
    push_log("Unable to fetch initial node height");
  }

  std::atomic<uint64_t> chain_event_version{0};
  std::atomic<bool> event_stream_active{false};

  NodeClient event_client(config_.node_host, config_.node_port);
  std::thread event_thread;

  if (config_.use_chain_events) {
    event_thread = std::thread([&]() {
      bool has_connected_once = false;
      bool interruption_reported = false;

      while (!stop_.load(std::memory_order_relaxed)) {
        if (!event_stream_active.load(std::memory_order_relaxed)) {
          try {
            event_client.disconnect();
            event_client.connect();
            event_client.subscribe_chain_events();
            event_stream_active.store(true, std::memory_order_relaxed);

            if (interruption_reported) {
              interruption_reported = false;
              push_log("Chain event stream restored");
              set_status("Mining");
            }
            has_connected_once = true;
          } catch (...) {
            if (!interruption_reported) {
              interruption_reported = true;
              if (has_connected_once) {
                set_status("Chain event stream interrupted, polling");
                push_log("Chain event stream interrupted, retrying");
              } else {
                set_status("Chain events unavailable, fallback polling");
                push_log("Chain events unavailable, using polling");
              }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
          }
        }

        try {
          const auto event = event_client.wait_chain_event();
          if (event.kind == ChainEventKind::BLOCK) {
            chain_event_version.fetch_add(1, std::memory_order_relaxed);
          }
        } catch (...) {
          if (stop_.load(std::memory_order_relaxed)) {
            return;
          }
          event_stream_active.store(false, std::memory_order_relaxed);
          event_client.disconnect();
        }
      }
    });
  }

  set_status("Connected (solo)");

  while (!stop_.load(std::memory_order_relaxed)) {
    if (paused_.load(std::memory_order_relaxed)) {
      set_status("Paused");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    set_status("Building solo job");
    MiningJob job = build_solo_job(client);

    std::atomic<bool> stale{false};
    std::atomic<bool> stop_watcher{false};
    const auto initial_height = current_height_.load(std::memory_order_relaxed);
    const auto initial_version = chain_event_version.load(std::memory_order_relaxed);
    const auto poll_interval = std::chrono::milliseconds(
      std::max<uint64_t>(100ULL, config_.refresh_interval_ms == 0 ? 500ULL : config_.refresh_interval_ms));
    std::thread stale_watcher([&]() {
      auto last_height_poll = std::chrono::steady_clock::now();
      while (!stop_watcher.load(std::memory_order_relaxed) &&
             !stale.load(std::memory_order_relaxed) &&
             !stop_.load(std::memory_order_relaxed)) {
        if (config_.use_chain_events && event_stream_active.load(std::memory_order_relaxed)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
          const auto now = std::chrono::steady_clock::now();
          if (chain_event_version.load(std::memory_order_relaxed) != initial_version) {
            stale.store(true, std::memory_order_relaxed);
            set_status("New block detected (events)");
            return;
          }
          if (now - last_height_poll >= std::chrono::milliseconds(1000)) {
            try {
              current_height_.store(client.height(), std::memory_order_relaxed);
            } catch (...) {
            }
            last_height_poll = now;
          }
          continue;
        }

        std::this_thread::sleep_for(poll_interval);
        try {
          const auto height = client.height();
          current_height_.store(height, std::memory_order_relaxed);
          if (height > initial_height) {
            stale.store(true, std::memory_order_relaxed);
            set_status("New block detected (poll)");
            return;
          }
        } catch (...) {
          set_status("Polling failed, keeping current job");
          continue;
        }
      }
    });

    set_status("Mining");
    const auto result = mine_block(job, stale);

    stop_watcher.store(true, std::memory_order_relaxed);
    stale_watcher.join();

    if (result.found && !stale.load(std::memory_order_relaxed)) {
      submit_candidate(client, job, result);
    }
  }

  event_stream_active.store(false, std::memory_order_relaxed);
  event_client.disconnect();
  if (event_thread.joinable()) {
    event_thread.join();
  }
  client.disconnect();
}

void Miner::run_pool() {
  NodeClient submit_client(config_.pool_host, config_.pool_port);
  NodeClient event_client(config_.pool_host, config_.pool_port);
  NodeClient node_height_client(config_.node_host, config_.node_port);

  submit_client.connect();
  push_log("Connected to pool " + config_.pool_host + ":" + std::to_string(config_.pool_port));

  const auto pool_diff_submit = submit_client.initialize_pool_handshake(miner_public_);

  std::atomic<bool> node_height_available{false};
  std::thread node_height_thread([&]() {
    bool has_connected_once = false;
    bool unavailable_reported = false;
    bool logged_initial_height = false;
    const auto poll_interval = std::chrono::milliseconds(
      std::max<uint64_t>(250ULL, config_.refresh_interval_ms == 0 ? 500ULL : config_.refresh_interval_ms));

    while (!stop_.load(std::memory_order_relaxed)) {
      try {
        node_height_client.disconnect();
        node_height_client.connect();

        if (has_connected_once && unavailable_reported) {
          unavailable_reported = false;
          push_log("Node height source restored");
        }
        has_connected_once = true;

        while (!stop_.load(std::memory_order_relaxed)) {
          const auto height = node_height_client.height();
          current_height_.store(height, std::memory_order_relaxed);
          node_height_available.store(true, std::memory_order_relaxed);
          if (!logged_initial_height) {
            logged_initial_height = true;
            push_log("Node height: " + std::to_string(height));
          }
          std::this_thread::sleep_for(poll_interval);
        }
      } catch (...) {
        node_height_available.store(false, std::memory_order_relaxed);
        node_height_client.disconnect();
        if (!unavailable_reported) {
          unavailable_reported = true;
          push_log("Node height query unavailable in pool mode, retrying");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }
    }
    node_height_client.disconnect();
  });

  current_tx_difficulty_.store(0.0, std::memory_order_relaxed);
  set_status("Connected (pool)");

  std::mutex job_mutex;
  std::optional<Block> latest_job;
  std::atomic<uint64_t> job_version{0};
  std::atomic<bool> event_stream_active{false};

  std::thread event_thread([&]() {
    bool has_connected_once = false;
    bool interruption_reported = false;

    while (!stop_.load(std::memory_order_relaxed)) {
      if (!event_stream_active.load(std::memory_order_relaxed)) {
        try {
          event_client.disconnect();
          event_client.connect();
          (void)event_client.initialize_pool_handshake(miner_public_);
          event_client.subscribe_chain_events();
          event_stream_active.store(true, std::memory_order_relaxed);

          if (interruption_reported) {
            interruption_reported = false;
            push_log("Pool event stream restored");
            set_status("Mining shares");
          } else if (!has_connected_once) {
            push_log("Subscribed to pool job stream");
          }
          has_connected_once = true;
        } catch (...) {
          if (!interruption_reported) {
            interruption_reported = true;
            if (has_connected_once) {
              set_status("Pool stream interrupted, reconnecting");
              push_log("Pool event stream interrupted, retrying");
            } else {
              set_status("Pool stream unavailable, reconnecting");
              push_log("Pool event stream unavailable, retrying");
            }
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1000));
          continue;
        }
      }

      try {
        const auto event = event_client.wait_chain_event();
        if (event.kind == ChainEventKind::BLOCK && event.block.has_value()) {
          {
            std::lock_guard<std::mutex> lock(job_mutex);
            latest_job = *event.block;
          }
          const auto version = job_version.fetch_add(1, std::memory_order_relaxed) + 1ULL;
          if (version == 1) {
            push_log("Initial pool job received");
          } else {
            push_log("Pool job refreshed");
          }
          set_status("New pool job");
        }
      } catch (...) {
        if (stop_.load(std::memory_order_relaxed)) {
          return;
        }
        event_stream_active.store(false, std::memory_order_relaxed);
        event_client.disconnect();
        if (!interruption_reported) {
          interruption_reported = true;
          set_status("Pool stream interrupted, reconnecting");
          push_log("Pool event stream interrupted, retrying");
        }
      }
    }
  });

  uint64_t prepared_job_version = 0;
  bool prepared_job_valid = false;
  MiningJob prepared_job;
  DifficultyTarget prepared_network_target{};

  while (!stop_.load(std::memory_order_relaxed)) {
    if (paused_.load(std::memory_order_relaxed)) {
      set_status("Paused");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    if (job_version.load(std::memory_order_relaxed) == 0) {
      if (!event_stream_active.load(std::memory_order_relaxed)) {
        set_status("Waiting for pool stream");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      } else {
        if (node_height_available.load(std::memory_order_relaxed)) {
          set_status("Waiting for first pool job");
        } else {
          set_status("Waiting for first pool job (node height unavailable)");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
      continue;
    }

    if (stop_.load(std::memory_order_relaxed)) {
      break;
    }

    Block block;
    uint64_t active_job_version = 0;
    {
      std::lock_guard<std::mutex> lock(job_mutex);
      if (!latest_job.has_value()) {
        continue;
      }
      block = *latest_job;
      active_job_version = job_version.load(std::memory_order_relaxed);
    }

    if (active_job_version == 0) {
      continue;
    }

    if (!prepared_job_valid || prepared_job_version != active_job_version) {
      prepared_job = build_pool_job(block, pool_diff_submit);
      prepared_network_target = calculate_block_difficulty_target(
        prepared_job.block.meta.block_pow_difficulty,
        prepared_job.block.transactions.size());
      prepared_job_version = active_job_version;
      prepared_job_valid = true;
    }

    MiningJob job = prepared_job;
    job.base_nonce = next_nonce_base();

    set_status("Mining shares");
    std::atomic<bool> stale{false};
    const auto result = mine_block(job, stale, &job_version, active_job_version);

    if (!result.found) {
      continue;
    }

    if (job_version.load(std::memory_order_relaxed) == active_job_version) {
      try {
        const bool full_block_candidate = hash_meets_target(result.hash, prepared_network_target);
        const bool accepted = submit_candidate(submit_client, job, result);
        if (accepted) {
          if (full_block_candidate) {
            push_log("Pool accepted full block candidate (round win check pending)");
          } else {
            push_log("Pool accepted share");
          }
        } else {
          if (full_block_candidate) {
            push_log("Pool rejected full block candidate");
          } else {
            push_log("Pool rejected share");
          }
        }
      } catch (const std::exception& ex) {
        set_status("Submit failed, retrying");
        push_log(std::string("Pool submit failed: ") + ex.what());
      } catch (...) {
        set_status("Submit failed, retrying");
        push_log("Pool submit failed: unknown error");
      }
    }
  }

  event_stream_active.store(false, std::memory_order_relaxed);
  node_height_available.store(false, std::memory_order_relaxed);
  event_client.disconnect();
  submit_client.disconnect();
  node_height_client.disconnect();
  if (event_thread.joinable()) {
    event_thread.join();
  }
  if (node_height_thread.joinable()) {
    node_height_thread.join();
  }
}

void Miner::run() {
  HashingConfig hash_cfg{};
  hash_cfg.full_mem = config_.randomx_full_mem;
  hash_cfg.huge_pages = config_.randomx_huge_pages;
  hash_cfg.jit = config_.randomx_jit;
  hash_cfg.hard_aes = config_.randomx_hard_aes;
  hash_cfg.secure = config_.randomx_secure;
  const HashingConfig requested_hash_cfg = hash_cfg;

  initialize_hashing(hash_cfg, config_.threads);
  const auto runtime_hash = hashing_runtime_profile();
  if (runtime_hash.randomx) {
    config_.randomx_full_mem = runtime_hash.full_mem;
    config_.randomx_huge_pages = runtime_hash.huge_pages;
    config_.randomx_jit = runtime_hash.jit;
    config_.randomx_hard_aes = runtime_hash.hard_aes;
    config_.randomx_secure = runtime_hash.secure;
    if (requested_hash_cfg.hard_aes && !runtime_hash.hard_aes) {
      push_log("RandomX fallback: HARD_AES requested but disabled by runtime initialization");
    }
    if (requested_hash_cfg.jit && !runtime_hash.jit) {
      push_log("RandomX fallback: JIT requested but disabled by runtime initialization");
    }
    if (requested_hash_cfg.huge_pages && !runtime_hash.huge_pages) {
      push_log("RandomX fallback: huge pages requested but not active");
    }
    if (requested_hash_cfg.full_mem && !runtime_hash.full_mem) {
      push_log("RandomX fallback: full memory requested but light mode is active");
    }
  } else {
    config_.randomx_full_mem = false;
    config_.randomx_huge_pages = false;
    config_.randomx_jit = false;
    config_.randomx_hard_aes = false;
    config_.randomx_secure = false;
  }

  if (config_.randomx_pipeline_batch == 0) {
    config_.randomx_pipeline_batch = current_pipeline_batch();
  }
  maybe_auto_tune_startup();

  start_time_ = std::chrono::steady_clock::now();
  shutdown_ui_.store(false, std::memory_order_relaxed);
  std::thread ui_thread([this]() { refresh_stats_loop(); });
  std::thread wallet_thread([this]() {
    NodeClient wallet_client(config_.node_host, config_.node_port);
    bool unavailable_reported = false;
    bool connected = false;
    bool logged_initial = false;
    const auto poll_interval = std::chrono::milliseconds(5000);

    while (!shutdown_ui_.load(std::memory_order_relaxed)) {
      try {
        if (!connected) {
          wallet_client.connect();
          connected = true;
        }
        const auto balance_nano = wallet_client.balance(miner_public_);
        wallet_balance_nano_.store(balance_nano, std::memory_order_relaxed);
        wallet_balance_available_.store(true, std::memory_order_relaxed);

        if (!wallet_baseline_set_.load(std::memory_order_relaxed)) {
          wallet_baseline_nano_.store(balance_nano, std::memory_order_relaxed);
          wallet_delta_nano_.store(0, std::memory_order_relaxed);
          wallet_baseline_set_.store(true, std::memory_order_relaxed);
        } else {
          const auto baseline = wallet_baseline_nano_.load(std::memory_order_relaxed);
          const auto delta =
            static_cast<int64_t>(balance_nano) - static_cast<int64_t>(baseline);
          wallet_delta_nano_.store(delta, std::memory_order_relaxed);
        }

        if (!logged_initial) {
          push_log(
            "Wallet balance tracking enabled: node=" +
            config_.node_host + ":" + std::to_string(config_.node_port) +
            " address=" + config_.wallet_address);
          logged_initial = true;
        }
        unavailable_reported = false;

        for (auto slept = std::chrono::milliseconds(0);
             slept < poll_interval && !shutdown_ui_.load(std::memory_order_relaxed);
             slept += std::chrono::milliseconds(100)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      } catch (...) {
        wallet_balance_available_.store(false, std::memory_order_relaxed);
        connected = false;
        wallet_client.disconnect();
        if (!unavailable_reported) {
          unavailable_reported = true;
          push_log("Wallet balance query unavailable, retrying");
        }
        for (auto slept = std::chrono::milliseconds(0);
             slept < std::chrono::milliseconds(1000) &&
               !shutdown_ui_.load(std::memory_order_relaxed);
             slept += std::chrono::milliseconds(100)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    }

    wallet_client.disconnect();
  });

  try {
    ensure_mining_workers_started();
    if (config_.mode == "solo") {
      run_solo();
    } else {
      run_pool();
    }
  } catch (...) {
    shutdown_ui_.store(true, std::memory_order_relaxed);
    stop_mining_workers();
    if (wallet_thread.joinable()) {
      wallet_thread.join();
    }
    if (ui_thread.joinable()) {
      ui_thread.join();
    }
    shutdown_hashing();
    throw;
  }

  shutdown_ui_.store(true, std::memory_order_relaxed);
  stop_mining_workers();
  if (wallet_thread.joinable()) {
    wallet_thread.join();
  }
  if (ui_thread.joinable()) {
    ui_thread.join();
  }
  shutdown_hashing();
}

} // namespace scrig
