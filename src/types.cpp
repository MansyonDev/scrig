#include "scrig/types.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string_view>

namespace scrig {

namespace {

constexpr std::array<uint8_t, 32> kDevWalletBytes = {
  237, 16, 162, 56, 254, 203, 62, 193, 77, 162, 64, 178, 25, 226, 137, 184,
  77, 191, 219, 2, 54, 178, 222, 164, 139, 138, 195, 169, 96, 66, 159, 155,
};

constexpr char kBase36Digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

int base36_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'z') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'Z') {
    return 10 + (c - 'A');
  }
  return -1;
}

std::string encode_base36(const uint8_t* data, size_t len) {
  std::vector<uint8_t> value(data, data + len);

  while (!value.empty() && value.front() == 0) {
    value.erase(value.begin());
  }

  if (value.empty()) {
    return "0";
  }

  std::string out;
  out.reserve(64);

  while (!value.empty()) {
    uint32_t remainder = 0;
    std::vector<uint8_t> quotient;
    quotient.reserve(value.size());

    for (const auto byte : value) {
      const uint32_t acc = (remainder * 256U) + static_cast<uint32_t>(byte);
      const uint8_t q = static_cast<uint8_t>(acc / 36U);
      remainder = acc % 36U;
      if (!quotient.empty() || q != 0) {
        quotient.push_back(q);
      }
    }

    out.push_back(kBase36Digits[remainder]);
    value = std::move(quotient);
  }

  std::reverse(out.begin(), out.end());
  return out;
}

template <size_t N>
std::array<uint8_t, N> decode_base36(std::string_view text) {
  if (text.empty()) {
    throw std::runtime_error("base36 string is empty");
  }

  std::vector<uint8_t> value(1, 0);

  for (const auto c : text) {
    const int digit = base36_digit(c);
    if (digit < 0) {
      throw std::runtime_error("invalid base36 character");
    }

    uint32_t carry = static_cast<uint32_t>(digit);
    for (size_t i = value.size(); i > 0; --i) {
      const uint32_t acc = static_cast<uint32_t>(value[i - 1]) * 36U + carry;
      value[i - 1] = static_cast<uint8_t>(acc & 0xFFU);
      carry = acc >> 8U;
    }

    while (carry > 0) {
      value.insert(value.begin(), static_cast<uint8_t>(carry & 0xFFU));
      carry >>= 8U;
    }
  }

  while (!value.empty() && value.front() == 0) {
    value.erase(value.begin());
  }

  if (value.size() > N) {
    throw std::runtime_error("base36 value is too large");
  }

  std::array<uint8_t, N> out{};
  const size_t offset = N - value.size();
  for (size_t i = 0; i < value.size(); ++i) {
    out[offset + i] = value[i];
  }
  return out;
}

const JsonValue& require_field(const JsonValue::object& obj, const std::string& key) {
  const auto it = obj.find(key);
  if (it == obj.end()) {
    throw std::runtime_error("missing field: " + key);
  }
  return it->second;
}

const JsonValue* optional_field(const JsonValue::object& obj, const std::string& key) {
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return nullptr;
  }
  return &it->second;
}

std::optional<Hash> optional_hash_from_json(const JsonValue& value) {
  if (value.is_null()) {
    return std::nullopt;
  }
  return hash_from_json(value);
}

JsonValue optional_hash_to_json(const std::optional<Hash>& value) {
  if (!value.has_value()) {
    return JsonValue(nullptr);
  }
  return hash_to_json(*value);
}

std::optional<Signature> optional_signature_from_json(const JsonValue& value) {
  if (value.is_null()) {
    return std::nullopt;
  }
  return signature_from_json(value);
}

JsonValue optional_signature_to_json(const std::optional<Signature>& value) {
  if (!value.has_value()) {
    return JsonValue(nullptr);
  }
  return signature_to_json(*value);
}

} // namespace

Hash Hash::zero() { return Hash{}; }

bool Hash::operator==(const Hash& other) const { return bytes == other.bytes; }
bool Hash::operator!=(const Hash& other) const { return !(*this == other); }
bool Hash::operator<(const Hash& other) const {
  return std::lexicographical_compare(bytes.begin(), bytes.end(), other.bytes.begin(), other.bytes.end());
}
bool Hash::operator<=(const Hash& other) const {
  return *this < other || *this == other;
}

bool PublicKey::operator==(const PublicKey& other) const { return bytes == other.bytes; }
bool Signature::operator==(const Signature& other) const { return bytes == other.bytes; }

bool TransactionInput::operator==(const TransactionInput& other) const {
  return transaction_id == other.transaction_id &&
         output_index == other.output_index &&
         signature == other.signature &&
         output_owner == other.output_owner;
}

bool TransactionOutput::operator==(const TransactionOutput& other) const {
  return amount == other.amount && receiver == other.receiver;
}

bool Transaction::operator==(const Transaction& other) const {
  return inputs == other.inputs && outputs == other.outputs &&
         transaction_id == other.transaction_id && nonce == other.nonce &&
         timestamp == other.timestamp;
}

bool AddressInclusionFilter::operator==(const AddressInclusionFilter& other) const {
  return bits == other.bits && num_bits == other.num_bits && num_hashes == other.num_hashes;
}

bool BlockMetadata::operator==(const BlockMetadata& other) const {
  return block_pow_difficulty == other.block_pow_difficulty &&
         tx_pow_difficulty == other.tx_pow_difficulty &&
         previous_block == other.previous_block &&
         hash == other.hash &&
         merkle_tree_root == other.merkle_tree_root &&
         address_inclusion_filter == other.address_inclusion_filter;
}

bool Block::operator==(const Block& other) const {
  return transactions == other.transactions &&
         timestamp == other.timestamp &&
         nonce == other.nonce &&
         meta == other.meta;
}

std::string hash_to_base36(const Hash& hash) {
  return encode_base36(hash.bytes.data(), hash.bytes.size());
}

Hash hash_from_base36(const std::string& base36) {
  return Hash{decode_base36<32>(base36)};
}

std::string public_key_to_base36(const PublicKey& key) {
  return encode_base36(key.bytes.data(), key.bytes.size());
}

PublicKey public_key_from_base36(const std::string& base36) {
  return PublicKey{decode_base36<32>(base36)};
}

std::string signature_to_base36(const Signature& signature) {
  return encode_base36(signature.bytes.data(), signature.bytes.size());
}

Signature signature_from_base36(const std::string& base36) {
  return Signature{decode_base36<64>(base36)};
}

JsonValue hash_to_json(const Hash& hash) {
  return JsonValue(hash_to_base36(hash));
}

Hash hash_from_json(const JsonValue& value) {
  return hash_from_base36(value.as_string());
}

JsonValue public_key_to_json(const PublicKey& key) {
  return JsonValue(public_key_to_base36(key));
}

PublicKey public_key_from_json(const JsonValue& value) {
  return public_key_from_base36(value.as_string());
}

JsonValue signature_to_json(const Signature& signature) {
  return JsonValue(signature_to_base36(signature));
}

Signature signature_from_json(const JsonValue& value) {
  return signature_from_base36(value.as_string());
}

JsonValue difficulty_target_to_json(const DifficultyTarget& target) {
  JsonValue::array arr;
  arr.reserve(target.size());
  for (const auto byte : target) {
    arr.emplace_back(static_cast<uint64_t>(byte));
  }
  return JsonValue(std::move(arr));
}

DifficultyTarget difficulty_target_from_json(const JsonValue& value) {
  const auto& arr = value.as_array();
  if (arr.size() != 32) {
    throw std::runtime_error("difficulty target must have 32 bytes");
  }
  DifficultyTarget target{};
  for (size_t i = 0; i < 32; ++i) {
    target[i] = static_cast<uint8_t>(arr[i].as_uint64());
  }
  return target;
}

JsonValue transaction_to_json(const Transaction& tx) {
  JsonValue::array inputs;
  inputs.reserve(tx.inputs.size());
  for (const auto& input : tx.inputs) {
    JsonValue::object input_obj{
      {"transaction_id", hash_to_json(input.transaction_id)},
      {"output_index", JsonValue(input.output_index)},
      {"signature", optional_signature_to_json(input.signature)},
      {"output_owner", public_key_to_json(input.output_owner)},
    };
    inputs.emplace_back(std::move(input_obj));
  }

  JsonValue::array outputs;
  outputs.reserve(tx.outputs.size());
  for (const auto& output : tx.outputs) {
    JsonValue::object output_obj{
      {"amount", JsonValue(output.amount)},
      {"receiver", public_key_to_json(output.receiver)},
    };
    outputs.emplace_back(std::move(output_obj));
  }

  JsonValue::object obj{
    {"inputs", JsonValue(std::move(inputs))},
    {"outputs", JsonValue(std::move(outputs))},
    {"transaction_id", optional_hash_to_json(tx.transaction_id)},
    {"nonce", JsonValue(tx.nonce)},
    {"timestamp", JsonValue(tx.timestamp)},
  };

  return JsonValue(std::move(obj));
}

Transaction transaction_from_json(const JsonValue& value) {
  const auto& obj = value.as_object();

  Transaction tx;

  const auto& input_arr = require_field(obj, "inputs").as_array();
  tx.inputs.reserve(input_arr.size());
  for (const auto& input_value : input_arr) {
    const auto& input_obj = input_value.as_object();

    TransactionInput input;
    input.transaction_id = hash_from_json(require_field(input_obj, "transaction_id"));
    input.output_index = require_field(input_obj, "output_index").as_uint64();
    input.signature = optional_signature_from_json(require_field(input_obj, "signature"));
    input.output_owner = public_key_from_json(require_field(input_obj, "output_owner"));

    tx.inputs.push_back(std::move(input));
  }

  const auto& output_arr = require_field(obj, "outputs").as_array();
  tx.outputs.reserve(output_arr.size());
  for (const auto& output_value : output_arr) {
    const auto& output_obj = output_value.as_object();

    TransactionOutput output;
    output.amount = require_field(output_obj, "amount").as_uint64();
    output.receiver = public_key_from_json(require_field(output_obj, "receiver"));

    tx.outputs.push_back(std::move(output));
  }

  tx.transaction_id = optional_hash_from_json(require_field(obj, "transaction_id"));
  tx.nonce = require_field(obj, "nonce").as_uint64();
  tx.timestamp = require_field(obj, "timestamp").as_uint64();

  return tx;
}

JsonValue address_filter_to_json(const AddressInclusionFilter& filter) {
  JsonValue::array bits;
  bits.reserve(filter.bits.size());
  for (const auto byte : filter.bits) {
    bits.emplace_back(static_cast<uint64_t>(byte));
  }

  JsonValue::object obj{
    {"bits", JsonValue(std::move(bits))},
    {"num_bits", JsonValue(filter.num_bits)},
    {"num_hashes", JsonValue(static_cast<uint64_t>(filter.num_hashes))},
  };

  return JsonValue(std::move(obj));
}

AddressInclusionFilter address_filter_from_json(const JsonValue& value) {
  const auto& obj = value.as_object();

  AddressInclusionFilter filter;
  filter.num_bits = require_field(obj, "num_bits").as_uint64();
  filter.num_hashes = static_cast<uint32_t>(require_field(obj, "num_hashes").as_uint64());

  const auto& bits = require_field(obj, "bits").as_array();
  filter.bits.reserve(bits.size());
  for (const auto& entry : bits) {
    filter.bits.push_back(static_cast<uint8_t>(entry.as_uint64()));
  }

  return filter;
}

JsonValue block_to_json(const Block& block) {
  JsonValue::array txs;
  txs.reserve(block.transactions.size());
  for (const auto& tx : block.transactions) {
    txs.push_back(transaction_to_json(tx));
  }

  JsonValue::array merkle_root;
  merkle_root.reserve(32);
  for (const auto byte : block.meta.merkle_tree_root) {
    merkle_root.emplace_back(static_cast<uint64_t>(byte));
  }

  JsonValue::object meta{
    {"block_pow_difficulty", difficulty_target_to_json(block.meta.block_pow_difficulty)},
    {"tx_pow_difficulty", difficulty_target_to_json(block.meta.tx_pow_difficulty)},
    {"previous_block", hash_to_json(block.meta.previous_block)},
    {"hash", optional_hash_to_json(block.meta.hash)},
    {"merkle_tree_root", JsonValue(std::move(merkle_root))},
    {"address_inclusion_filter", address_filter_to_json(block.meta.address_inclusion_filter)},
  };

  JsonValue::object obj{
    {"transactions", JsonValue(std::move(txs))},
    {"timestamp", JsonValue(block.timestamp)},
    {"nonce", JsonValue(block.nonce)},
    {"meta", JsonValue(std::move(meta))},
  };

  return JsonValue(std::move(obj));
}

Block block_from_json(const JsonValue& value) {
  const auto& obj = value.as_object();

  Block block;
  block.timestamp = require_field(obj, "timestamp").as_uint64();
  block.nonce = require_field(obj, "nonce").as_uint64();

  const auto& tx_values = require_field(obj, "transactions").as_array();
  block.transactions.reserve(tx_values.size());
  for (const auto& tx : tx_values) {
    block.transactions.push_back(transaction_from_json(tx));
  }

  const auto& meta = require_field(obj, "meta").as_object();
  block.meta.block_pow_difficulty = difficulty_target_from_json(require_field(meta, "block_pow_difficulty"));
  block.meta.tx_pow_difficulty = difficulty_target_from_json(require_field(meta, "tx_pow_difficulty"));
  block.meta.previous_block = hash_from_json(require_field(meta, "previous_block"));
  block.meta.hash = optional_hash_from_json(require_field(meta, "hash"));

  const auto& merkle_root = require_field(meta, "merkle_tree_root").as_array();
  if (merkle_root.size() != 32) {
    throw std::runtime_error("merkle_tree_root must have 32 bytes");
  }
  for (size_t i = 0; i < 32; ++i) {
    block.meta.merkle_tree_root[i] = static_cast<uint8_t>(merkle_root[i].as_uint64());
  }

  block.meta.address_inclusion_filter = address_filter_from_json(require_field(meta, "address_inclusion_filter"));
  return block;
}

ChainEvent chain_event_from_json(const JsonValue& value) {
  const auto& obj = value.as_object();
  ChainEvent event;

  if (const auto* block_value = optional_field(obj, "Block"); block_value != nullptr) {
    const auto& block_obj = block_value->as_object();
    event.kind = ChainEventKind::BLOCK;
    event.block = block_from_json(require_field(block_obj, "block"));
    return event;
  }

  if (const auto* tx_value = optional_field(obj, "Transaction"); tx_value != nullptr) {
    const auto& tx_obj = tx_value->as_object();
    event.kind = ChainEventKind::TRANSACTION;
    event.transaction = transaction_from_json(require_field(tx_obj, "transaction"));
    return event;
  }

  if (const auto* tx_exp = optional_field(obj, "TransactionExpiration"); tx_exp != nullptr) {
    const auto& tx_exp_obj = tx_exp->as_object();
    event.kind = ChainEventKind::TRANSACTION_EXPIRATION;
    event.transaction_id = hash_from_json(require_field(tx_exp_obj, "transaction"));
    return event;
  }

  event.kind = ChainEventKind::UNKNOWN;
  return event;
}

uint64_t unix_timestamp_now() {
  const auto now = std::chrono::system_clock::now();
  const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
  return static_cast<uint64_t>(epoch.count());
}

uint64_t get_block_reward(uint64_t height) {
  const uint64_t halvings = height / SNAP_HALVING_INTERVAL;
  uint64_t reward = SNAP_INITIAL_REWARD;
  if (halvings >= 63) {
    reward = 0;
  } else {
    reward >>= halvings;
  }
  return std::max<uint64_t>(reward, SNAP_MIN_REWARD);
}

uint64_t calculate_dev_fee(uint64_t block_reward) {
  return static_cast<uint64_t>(static_cast<double>(block_reward) * SNAP_DEV_FEE);
}

Hash genesis_previous_block_hash() {
  return Hash::zero();
}

PublicKey dev_wallet() {
  return PublicKey{kDevWalletBytes};
}

} // namespace scrig

