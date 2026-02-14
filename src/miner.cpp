#include "scrig/miner.hpp"

#include "scrig/consensus.hpp"
#include "scrig/perf.hpp"
#include "scrig/ui.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <mutex>
#include <random>
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

} // namespace

Miner::Miner(Config config)
  : config_(std::move(config)),
    miner_public_(public_key_from_base36(config_.wallet_address)) {}

void Miner::request_stop() {
  stop_.store(true, std::memory_order_relaxed);
}

void Miner::add_runtime_note(std::string note) {
  push_log(std::move(note));
}

void Miner::push_log(std::string line) {
  std::lock_guard<std::mutex> lock(log_mutex_);
  runtime_logs_.push_back(std::move(line));
}

void Miner::set_status(const std::string& status) {
  std::lock_guard<std::mutex> lock(status_mutex_);
  status_ = status;
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
  std::random_device rd;
  std::mt19937_64 rng(rd());
  job.base_nonce = 0x100000000ULL | (rng() & 0xFFFFFFFFULL);
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
  std::random_device rd;
  std::mt19937_64 rng(rd());
  job.base_nonce = 0x100000000ULL | (rng() & 0xFFFFFFFFULL);

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
        auto local_buffer = pow_template;
        uint64_t nonce = reward_tx.nonce + worker_id;
        uint64_t local_hashes = 0;
        auto last_flush = std::chrono::steady_clock::now();
        const bool fast_nonce_patch = patch_nonce_in_pow_buffer(local_buffer, nonce_offset, nonce);
        uint8_t* nonce_ptr = fast_nonce_patch ? local_buffer.data() + nonce_offset + 1 : nullptr;
        Transaction slow_candidate = reward_tx;

        while (!stop_.load(std::memory_order_relaxed) && !found.load(std::memory_order_relaxed)) {
          Hash hash{};

          if (fast_nonce_patch) {
            for (size_t i = 0; i < 8; ++i) {
              nonce_ptr[i] = static_cast<uint8_t>((nonce >> (8U * i)) & 0xFFU);
            }
            hash = hash_data(local_buffer);
          } else {
            slow_candidate.nonce = nonce;
            hash = compute_transaction_hash(slow_candidate);
          }

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

          const auto now = std::chrono::steady_clock::now();
          if (local_hashes >= 64 ||
              std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count() >= 250) {
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

Miner::MiningResult Miner::mine_block(const MiningJob& job, std::atomic<bool>& cancel_signal) {
  MiningResult result;
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
        if (config_.pin_threads) {
          (void)pin_current_thread(worker_id, thread_count);
        }
        if (config_.numa_bind) {
          (void)bind_current_thread_numa(worker_id, thread_count);
        }

        auto local_buffer = job.block_pow_template;
        uint64_t nonce = job.base_nonce + worker_id;
        uint64_t local_hashes = 0;
        auto last_flush = std::chrono::steady_clock::now();
        const bool fast_nonce_patch = patch_nonce_in_pow_buffer(local_buffer, job.block_nonce_offset, nonce);
        uint8_t* nonce_ptr = nullptr;
        if (fast_nonce_patch) {
          nonce_ptr = local_buffer.data() + job.block_nonce_offset + 1;
        } else {
          nonce_ptr = nullptr;
        }
        Block slow_candidate = job.block;
        slow_candidate.meta.hash.reset();

        while (!stop_.load(std::memory_order_relaxed) &&
               !cancel_signal.load(std::memory_order_relaxed) &&
               !found.load(std::memory_order_relaxed)) {

          Hash hash{};

          if (fast_nonce_patch) {
            for (size_t i = 0; i < 8; ++i) {
              nonce_ptr[i] = static_cast<uint8_t>((nonce >> (8U * i)) & 0xFFU);
            }
            hash = hash_data(local_buffer);
          } else {
            slow_candidate.nonce = nonce;
            hash = compute_block_hash(slow_candidate);
          }

          ++local_hashes;

          if (hash_meets_target(hash, job.target)) {
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

          const auto now = std::chrono::steady_clock::now();
          if (local_hashes >= 64 ||
              std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count() >= 250) {
            total_hashes_.fetch_add(local_hashes, std::memory_order_relaxed);
            local_hashes = 0;
            last_flush = now;
          }
        }

        if (local_hashes > 0) {
          total_hashes_.fetch_add(local_hashes, std::memory_order_relaxed);
        }
      } catch (...) {
        cancel_signal.store(true, std::memory_order_relaxed);
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

  if (found.load(std::memory_order_relaxed)) {
    result.found = true;
    result.nonce = found_nonce.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(found_lock);
    result.hash = found_hash;
  }

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

void Miner::refresh_stats_loop() {
  uint64_t last_hashes = total_hashes_.load(std::memory_order_relaxed);
  auto last_tick = std::chrono::steady_clock::now();
  uint64_t last_summary_uptime = 0;

  while (!shutdown_ui_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::seconds(1));

    const auto now = std::chrono::steady_clock::now();
    const auto current_hashes = total_hashes_.load(std::memory_order_relaxed);
    const auto elapsed = std::chrono::duration<double>(now - last_tick).count();

    if (elapsed > 0.0) {
      const auto delta = static_cast<double>(current_hashes - last_hashes);
      const double instant_rate = delta / elapsed;
      const double prev_rate = current_hashrate_.load(std::memory_order_relaxed);
      const double smoothed_rate = (prev_rate <= 0.0)
        ? instant_rate
        : (prev_rate * 0.75 + instant_rate * 0.25);
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
    try {
      event_client.connect();
      event_client.subscribe_chain_events();
      event_stream_active.store(true, std::memory_order_relaxed);

      event_thread = std::thread([&]() {
        while (!stop_.load(std::memory_order_relaxed)) {
          try {
            const auto event = event_client.wait_chain_event();
            if (event.kind == ChainEventKind::BLOCK) {
              chain_event_version.fetch_add(1, std::memory_order_relaxed);
            }
          } catch (...) {
            event_stream_active.store(false, std::memory_order_relaxed);
            set_status("Chain event stream interrupted, fallback polling");
            push_log("Chain event stream interrupted, using polling");
            return;
          }
        }
      });
    } catch (...) {
      event_stream_active.store(false, std::memory_order_relaxed);
      set_status("Chain events unavailable, fallback polling");
      push_log("Chain events unavailable, using polling");
    }
  }

  set_status("Connected (solo)");

  while (!stop_.load(std::memory_order_relaxed)) {
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

  submit_client.connect();
  event_client.connect();
  push_log("Connected to pool " + config_.pool_host + ":" + std::to_string(config_.pool_port));

  const auto pool_diff_submit = submit_client.initialize_pool_handshake(miner_public_);
  const auto pool_diff_events = event_client.initialize_pool_handshake(miner_public_);
  (void)pool_diff_events;

  try {
    current_height_.store(submit_client.height(), std::memory_order_relaxed);
  } catch (...) {
    // Ignore height failures in pool mode; jobs are still event-driven.
  }

  current_tx_difficulty_.store(0.0, std::memory_order_relaxed);
  set_status("Connected (pool)");

  event_client.subscribe_chain_events();

  std::mutex job_mutex;
  std::optional<Block> latest_job;
  std::atomic<uint64_t> job_version{0};

  std::thread event_thread([&]() {
    while (!stop_.load(std::memory_order_relaxed)) {
      try {
        const auto event = event_client.wait_chain_event();
        if (event.kind == ChainEventKind::BLOCK && event.block.has_value()) {
          {
            std::lock_guard<std::mutex> lock(job_mutex);
            latest_job = *event.block;
          }
          try {
            current_height_.store(submit_client.height(), std::memory_order_relaxed);
          } catch (...) {
          }
          job_version.fetch_add(1, std::memory_order_relaxed);
          set_status("New pool job");
          push_log("Pool job received");
        }
      } catch (...) {
        if (!stop_.load(std::memory_order_relaxed)) {
          set_status("Pool event stream interrupted");
          push_log("Pool event stream interrupted");
          stop_.store(true, std::memory_order_relaxed);
        }
      }
    }
  });

  uint64_t local_job_version = 0;

  while (!stop_.load(std::memory_order_relaxed)) {
    while (!stop_.load(std::memory_order_relaxed) &&
           job_version.load(std::memory_order_relaxed) == local_job_version) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    if (stop_.load(std::memory_order_relaxed)) {
      break;
    }

    Block block;
    {
      std::lock_guard<std::mutex> lock(job_mutex);
      if (!latest_job.has_value()) {
        continue;
      }
      block = *latest_job;
    }

    local_job_version = job_version.load(std::memory_order_relaxed);
    MiningJob job = build_pool_job(block, pool_diff_submit);

    std::atomic<bool> stale{false};
    std::thread stale_watcher([&]() {
      while (!stale.load(std::memory_order_relaxed) && !stop_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (job_version.load(std::memory_order_relaxed) != local_job_version) {
          stale.store(true, std::memory_order_relaxed);
          set_status("Pool job refreshed");
          return;
        }
      }
    });

    set_status("Mining shares");
    const auto result = mine_block(job, stale);

    stale.store(true, std::memory_order_relaxed);
    stale_watcher.join();

    if (!result.found) {
      continue;
    }

    if (job_version.load(std::memory_order_relaxed) == local_job_version) {
      submit_candidate(submit_client, job, result);
    }
  }

  event_client.disconnect();
  submit_client.disconnect();
  if (event_thread.joinable()) {
    event_thread.join();
  }
}

void Miner::run() {
  HashingConfig hash_cfg{};
  hash_cfg.full_mem = config_.randomx_full_mem;
  hash_cfg.huge_pages = config_.randomx_huge_pages;
  hash_cfg.jit = config_.randomx_jit;
  hash_cfg.hard_aes = config_.randomx_hard_aes;
  hash_cfg.secure = config_.randomx_secure;

  initialize_hashing(hash_cfg, config_.threads);

  start_time_ = std::chrono::steady_clock::now();
  shutdown_ui_.store(false, std::memory_order_relaxed);
  std::thread ui_thread([this]() { refresh_stats_loop(); });

  try {
    if (config_.mode == "solo") {
      run_solo();
    } else {
      run_pool();
    }
  } catch (...) {
    shutdown_ui_.store(true, std::memory_order_relaxed);
    if (ui_thread.joinable()) {
      ui_thread.join();
    }
    shutdown_hashing();
    throw;
  }

  shutdown_ui_.store(true, std::memory_order_relaxed);
  if (ui_thread.joinable()) {
    ui_thread.join();
  }
  shutdown_hashing();
}

} // namespace scrig
