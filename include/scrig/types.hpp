#pragma once

#include "scrig/json.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scrig {

constexpr uint64_t SNAP_NANO_PER_COIN = 100000000ULL;
constexpr uint64_t SNAP_INITIAL_REWARD = 100ULL * SNAP_NANO_PER_COIN;
constexpr uint64_t SNAP_HALVING_INTERVAL = 1000000ULL;
constexpr uint64_t SNAP_MIN_REWARD = 1ULL;
constexpr double SNAP_DEV_FEE = 0.02;
constexpr uint64_t SNAP_MAX_TRANSACTIONS_PER_BLOCK = 500;
constexpr uint64_t SNAP_EXPIRATION_TIME_SECONDS = 35ULL * 10ULL;
constexpr double SNAP_DIFFICULTY_DECAY_PER_TRANSACTION = 0.005;
constexpr uint64_t SNAP_SCIP1_MIGRATION = 1770375600ULL;

using DifficultyTarget = std::array<uint8_t, 32>;

struct Hash {
  std::array<uint8_t, 32> bytes{};

  static Hash zero();
  bool operator==(const Hash& other) const;
  bool operator!=(const Hash& other) const;
  bool operator<(const Hash& other) const;
  bool operator<=(const Hash& other) const;
};

struct PublicKey {
  std::array<uint8_t, 32> bytes{};

  bool operator==(const PublicKey& other) const;
};

struct Signature {
  std::array<uint8_t, 64> bytes{};

  bool operator==(const Signature& other) const;
};

struct TransactionInput {
  Hash transaction_id = Hash::zero();
  uint64_t output_index = 0;
  std::optional<Signature> signature;
  PublicKey output_owner{};

  bool operator==(const TransactionInput& other) const;
};

struct TransactionOutput {
  uint64_t amount = 0;
  PublicKey receiver{};

  bool operator==(const TransactionOutput& other) const;
};

struct Transaction {
  std::vector<TransactionInput> inputs;
  std::vector<TransactionOutput> outputs;
  std::optional<Hash> transaction_id;
  uint64_t nonce = 0;
  uint64_t timestamp = 0;

  bool operator==(const Transaction& other) const;
};

struct AddressInclusionFilter {
  std::vector<uint8_t> bits;
  uint64_t num_bits = 0;
  uint32_t num_hashes = 0;

  bool operator==(const AddressInclusionFilter& other) const;
};

struct BlockMetadata {
  DifficultyTarget block_pow_difficulty{};
  DifficultyTarget tx_pow_difficulty{};
  Hash previous_block = Hash::zero();
  std::optional<Hash> hash;
  std::array<uint8_t, 32> merkle_tree_root{};
  AddressInclusionFilter address_inclusion_filter{};

  bool operator==(const BlockMetadata& other) const;
};

struct Block {
  std::vector<Transaction> transactions;
  uint64_t timestamp = 0;
  uint64_t nonce = 0;
  BlockMetadata meta{};

  bool operator==(const Block& other) const;
};

enum class ChainEventKind {
  UNKNOWN,
  BLOCK,
  TRANSACTION,
  TRANSACTION_EXPIRATION,
};

struct ChainEvent {
  ChainEventKind kind = ChainEventKind::UNKNOWN;
  std::optional<Block> block;
  std::optional<Transaction> transaction;
  std::optional<Hash> transaction_id;
};

struct DifficultyInfo {
  DifficultyTarget transaction_difficulty{};
  DifficultyTarget block_difficulty{};
};

struct MempoolPage {
  std::vector<Transaction> mempool;
  std::optional<uint32_t> next_page;
};

std::string hash_to_base36(const Hash& hash);
Hash hash_from_base36(const std::string& base36);

std::string public_key_to_base36(const PublicKey& key);
PublicKey public_key_from_base36(const std::string& base36);

std::string signature_to_base36(const Signature& signature);
Signature signature_from_base36(const std::string& base36);

JsonValue hash_to_json(const Hash& hash);
Hash hash_from_json(const JsonValue& value);

JsonValue public_key_to_json(const PublicKey& key);
PublicKey public_key_from_json(const JsonValue& value);

JsonValue signature_to_json(const Signature& signature);
Signature signature_from_json(const JsonValue& value);

JsonValue difficulty_target_to_json(const DifficultyTarget& target);
DifficultyTarget difficulty_target_from_json(const JsonValue& value);

JsonValue transaction_to_json(const Transaction& tx);
Transaction transaction_from_json(const JsonValue& value);

JsonValue address_filter_to_json(const AddressInclusionFilter& filter);
AddressInclusionFilter address_filter_from_json(const JsonValue& value);

JsonValue block_to_json(const Block& block);
Block block_from_json(const JsonValue& value);

ChainEvent chain_event_from_json(const JsonValue& value);

uint64_t unix_timestamp_now();
uint64_t get_block_reward(uint64_t height);
uint64_t calculate_dev_fee(uint64_t block_reward);
Hash genesis_previous_block_hash();
PublicKey dev_wallet();

} // namespace scrig

