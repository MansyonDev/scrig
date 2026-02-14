#pragma once

#include "scrig/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace scrig {

struct HashingConfig {
  bool full_mem = true;
  bool huge_pages = true;
  bool jit = true;
  bool hard_aes = true;
  bool secure = false;
};

struct HashingRuntimeProfile {
  bool randomx = false;
  bool initialized = false;
  bool full_mem = false;
  bool huge_pages = false;
  bool jit = false;
  bool hard_aes = false;
  bool secure = false;
};

void initialize_hashing(const HashingConfig& config, uint32_t init_threads);
void shutdown_hashing();
bool hashing_uses_randomx();
HashingRuntimeProfile hashing_runtime_profile();

Hash hash_data(std::span<const uint8_t> data);
Hash hash_data(const std::vector<uint8_t>& data);

DifficultyTarget calculate_block_difficulty_target(const DifficultyTarget& block_difficulty, size_t tx_count);
bool hash_meets_target(const Hash& hash, const DifficultyTarget& target);

std::vector<uint8_t> transaction_pow_buffer(const Transaction& tx, size_t* nonce_offset = nullptr);
Hash compute_transaction_hash(const Transaction& tx);
bool patch_nonce_in_pow_buffer(std::vector<uint8_t>& pow_buffer, size_t nonce_offset, uint64_t nonce);

std::vector<uint8_t> block_pow_buffer(const Block& block, size_t* nonce_offset = nullptr);
Hash compute_block_hash(const Block& block);

std::array<uint8_t, 32> build_merkle_root(const std::vector<Transaction>& txs);
AddressInclusionFilter build_address_filter(const std::vector<Transaction>& txs);

bool trim_expired_transactions(Block& block, uint64_t now_ts);

Transaction build_reward_transaction(
  const PublicKey& miner,
  uint64_t reward,
  uint64_t timestamp);

} // namespace scrig
