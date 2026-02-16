#include "scrig/consensus.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef SCRIG_HAVE_RANDOMX
#include <randomx.h>
#endif

#ifdef _MSC_VER
#include <intrin.h>
#if defined(_WIN32) && (defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86))
#include <windows.h>
#include <immintrin.h>
#endif
#endif

namespace scrig {

namespace {

constexpr double kAddressFilterFalsePositiveRate = 0.001;
#ifdef SCRIG_HAVE_RANDOMX
constexpr uint64_t kRandomXSeedLen = 17;
constexpr const char* kRandomXSeed = "snap-coin-testnet";
#endif

class BincodeWriter {
public:
  void write_u8(uint8_t value) { buffer_.push_back(value); }

  void write_bool(bool value) {
    write_u8(value ? 1U : 0U);
  }

  void write_fixed_bytes(const uint8_t* data, size_t len) {
    buffer_.insert(buffer_.end(), data, data + len);
  }

  void write_fixed_array32(const std::array<uint8_t, 32>& data) {
    write_fixed_bytes(data.data(), data.size());
  }

  void write_fixed_array64(const std::array<uint8_t, 64>& data) {
    write_fixed_bytes(data.data(), data.size());
  }

  void write_varuint(uint64_t value) {
    if (value < 251U) {
      write_u8(static_cast<uint8_t>(value));
      return;
    }

    if (value <= 0xFFFFULL) {
      write_u8(251U);
      write_le_u16(static_cast<uint16_t>(value));
      return;
    }

    if (value <= 0xFFFFFFFFULL) {
      write_u8(252U);
      write_le_u32(static_cast<uint32_t>(value));
      return;
    }

    write_u8(253U);
    write_le_u64(value);
  }

  void write_usize(size_t value) {
    write_varuint(static_cast<uint64_t>(value));
  }

  size_t write_varuint_placeholder_u64(uint64_t value) {
    const size_t offset = buffer_.size();
    write_varuint(value);
    return offset;
  }

  const std::vector<uint8_t>& buffer() const { return buffer_; }
  std::vector<uint8_t>& buffer() { return buffer_; }

private:
  void write_le_u16(uint16_t value) {
    buffer_.push_back(static_cast<uint8_t>(value & 0xFFU));
    buffer_.push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
  }

  void write_le_u32(uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
      buffer_.push_back(static_cast<uint8_t>((value >> (i * 8U)) & 0xFFU));
    }
  }

  void write_le_u64(uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
      buffer_.push_back(static_cast<uint8_t>((value >> (i * 8U)) & 0xFFU));
    }
  }

  std::vector<uint8_t> buffer_;
};

void patch_varuint_u64_in_place_9(std::vector<uint8_t>& buffer, size_t offset, uint64_t value) {
  // Fixed 9-byte varint representation (prefix 253 + u64 little-endian).
  if (offset + 9 > buffer.size()) {
    throw std::runtime_error("nonce offset is out of range");
  }
  buffer[offset] = 253U;
  for (size_t i = 0; i < 8; ++i) {
    buffer[offset + 1 + i] = static_cast<uint8_t>((value >> (i * 8U)) & 0xFFU);
  }
}

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len) {
  struct Sha256Ctx {
    std::array<uint32_t, 8> state{};
    std::array<uint8_t, 64> block{};
    uint64_t bit_length = 0;
    size_t block_len = 0;
  };

  constexpr std::array<uint32_t, 64> K = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
  };

  auto rotr = [](uint32_t x, uint32_t n) -> uint32_t {
    return (x >> n) | (x << (32U - n));
  };

  auto transform = [&](Sha256Ctx& ctx) {
    std::array<uint32_t, 64> w{};

    for (size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(ctx.block[i * 4]) << 24U) |
             (static_cast<uint32_t>(ctx.block[i * 4 + 1]) << 16U) |
             (static_cast<uint32_t>(ctx.block[i * 4 + 2]) << 8U) |
             static_cast<uint32_t>(ctx.block[i * 4 + 3]);
    }

    for (size_t i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx.state[0];
    uint32_t b = ctx.state[1];
    uint32_t c = ctx.state[2];
    uint32_t d = ctx.state[3];
    uint32_t e = ctx.state[4];
    uint32_t f = ctx.state[5];
    uint32_t g = ctx.state[6];
    uint32_t h = ctx.state[7];

    for (size_t i = 0; i < 64; ++i) {
      const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ ((~e) & g);
      const uint32_t temp1 = h + s1 + ch + K[i] + w[i];
      const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temp2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    ctx.state[0] += a;
    ctx.state[1] += b;
    ctx.state[2] += c;
    ctx.state[3] += d;
    ctx.state[4] += e;
    ctx.state[5] += f;
    ctx.state[6] += g;
    ctx.state[7] += h;
  };

  Sha256Ctx ctx;
  ctx.state = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };

  for (size_t i = 0; i < len; ++i) {
    ctx.block[ctx.block_len++] = data[i];
    if (ctx.block_len == 64) {
      transform(ctx);
      ctx.bit_length += 512;
      ctx.block_len = 0;
    }
  }

  size_t i = ctx.block_len;
  if (ctx.block_len < 56) {
    ctx.block[i++] = 0x80;
    while (i < 56) {
      ctx.block[i++] = 0;
    }
  } else {
    ctx.block[i++] = 0x80;
    while (i < 64) {
      ctx.block[i++] = 0;
    }
    transform(ctx);
    ctx.block.fill(0);
  }

  ctx.bit_length += static_cast<uint64_t>(ctx.block_len) * 8ULL;
  ctx.block[63] = static_cast<uint8_t>(ctx.bit_length);
  ctx.block[62] = static_cast<uint8_t>(ctx.bit_length >> 8U);
  ctx.block[61] = static_cast<uint8_t>(ctx.bit_length >> 16U);
  ctx.block[60] = static_cast<uint8_t>(ctx.bit_length >> 24U);
  ctx.block[59] = static_cast<uint8_t>(ctx.bit_length >> 32U);
  ctx.block[58] = static_cast<uint8_t>(ctx.bit_length >> 40U);
  ctx.block[57] = static_cast<uint8_t>(ctx.bit_length >> 48U);
  ctx.block[56] = static_cast<uint8_t>(ctx.bit_length >> 56U);
  transform(ctx);

  std::array<uint8_t, 32> out{};
  for (size_t j = 0; j < 4; ++j) {
    out[j] = static_cast<uint8_t>((ctx.state[0] >> (24U - j * 8U)) & 0xFFU);
    out[j + 4] = static_cast<uint8_t>((ctx.state[1] >> (24U - j * 8U)) & 0xFFU);
    out[j + 8] = static_cast<uint8_t>((ctx.state[2] >> (24U - j * 8U)) & 0xFFU);
    out[j + 12] = static_cast<uint8_t>((ctx.state[3] >> (24U - j * 8U)) & 0xFFU);
    out[j + 16] = static_cast<uint8_t>((ctx.state[4] >> (24U - j * 8U)) & 0xFFU);
    out[j + 20] = static_cast<uint8_t>((ctx.state[5] >> (24U - j * 8U)) & 0xFFU);
    out[j + 24] = static_cast<uint8_t>((ctx.state[6] >> (24U - j * 8U)) & 0xFFU);
    out[j + 28] = static_cast<uint8_t>((ctx.state[7] >> (24U - j * 8U)) & 0xFFU);
  }

  return out;
}

std::array<uint8_t, 32> multiply_divide_target(
  const std::array<uint8_t, 32>& input,
  uint64_t multiply,
  uint64_t divide,
  bool* overflowed) {

  std::array<uint8_t, 32> multiplied{};

#if defined(_MSC_VER)
  bool local_overflow = false;
  uint64_t carry = 0;

  for (size_t idx = 32; idx > 0; --idx) {
    const size_t i = idx - 1;
    unsigned __int64 hi = 0;
    const unsigned __int64 lo = _umul128(
      static_cast<unsigned __int64>(input[i]),
      static_cast<unsigned __int64>(multiply),
      &hi);

    const unsigned __int64 sum_lo = lo + carry;
    const unsigned __int64 sum_hi = hi + (sum_lo < lo ? 1ULL : 0ULL);
    multiplied[i] = static_cast<uint8_t>(sum_lo & 0xFFU);

    if (sum_hi > 0xFFU) {
      local_overflow = true;
      break;
    }

    carry = (sum_hi << 56U) | (sum_lo >> 8U);
  }

  if (overflowed != nullptr) {
    *overflowed = local_overflow || carry > 0;
  }

  if (local_overflow || carry > 0) {
    multiplied.fill(0xFFU);
    return multiplied;
  }

  std::array<uint8_t, 32> result{};
  uint64_t remainder = 0;
  for (size_t i = 0; i < 32; ++i) {
#if defined(_M_X64)
    const unsigned __int64 numerator_hi = remainder >> 56U;
    const unsigned __int64 numerator_lo = (remainder << 8U) | static_cast<unsigned __int64>(multiplied[i]);
    unsigned __int64 remainder_out = 0;
    const unsigned __int64 q = _udiv128(numerator_hi, numerator_lo, divide, &remainder_out);
    if (q > 0xFFU) {
      if (overflowed != nullptr) {
        *overflowed = true;
      }
      result.fill(0xFFU);
      return result;
    }
    result[i] = static_cast<uint8_t>(q);
    remainder = remainder_out;
#else
    // Conservative fallback for MSVC non-x64 targets.
    if (remainder > (UINT64_MAX >> 8U)) {
      if (overflowed != nullptr) {
        *overflowed = true;
      }
      result.fill(0xFFU);
      return result;
    }
    const uint64_t cur = (remainder << 8U) | static_cast<uint64_t>(multiplied[i]);
    result[i] = static_cast<uint8_t>(cur / divide);
    remainder = cur % divide;
#endif
  }

  return result;
#else
  unsigned __int128 carry = 0;

  for (size_t idx = 32; idx > 0; --idx) {
    const size_t i = idx - 1;
    const unsigned __int128 cur =
      static_cast<unsigned __int128>(input[i]) * static_cast<unsigned __int128>(multiply) + carry;
    multiplied[i] = static_cast<uint8_t>(cur & 0xFFU);
    carry = cur >> 8U;
  }

  if (overflowed != nullptr) {
    *overflowed = carry > 0;
  }

  if (carry > 0) {
    multiplied.fill(0xFFU);
    return multiplied;
  }

  std::array<uint8_t, 32> result{};
  unsigned __int128 remainder = 0;
  for (size_t i = 0; i < 32; ++i) {
    const unsigned __int128 cur = remainder * 256U + multiplied[i];
    result[i] = static_cast<uint8_t>(cur / divide);
    remainder = cur % divide;
  }

  return result;
#endif
}

void encode_hash(BincodeWriter& writer, const Hash& hash) {
  writer.write_fixed_array32(hash.bytes);
}

void encode_public(BincodeWriter& writer, const PublicKey& key) {
  writer.write_fixed_array32(key.bytes);
}

void encode_signature(BincodeWriter& writer, const Signature& signature) {
  writer.write_fixed_array64(signature.bytes);
}

void encode_option_hash(BincodeWriter& writer, const std::optional<Hash>& hash) {
  if (!hash.has_value()) {
    writer.write_varuint(0);
    return;
  }
  writer.write_varuint(1);
  encode_hash(writer, *hash);
}

void encode_option_signature(BincodeWriter& writer, const std::optional<Signature>& signature) {
  if (!signature.has_value()) {
    writer.write_varuint(0);
    return;
  }
  writer.write_varuint(1);
  encode_signature(writer, *signature);
}

void encode_tx_input(BincodeWriter& writer, const TransactionInput& input) {
  encode_hash(writer, input.transaction_id);
  writer.write_varuint(input.output_index);
  encode_option_signature(writer, input.signature);
  encode_public(writer, input.output_owner);
}

void encode_tx_output(BincodeWriter& writer, const TransactionOutput& output) {
  writer.write_varuint(output.amount);
  encode_public(writer, output.receiver);
}

void encode_transaction(BincodeWriter& writer, const Transaction& tx) {
  writer.write_usize(tx.inputs.size());
  for (const auto& input : tx.inputs) {
    encode_tx_input(writer, input);
  }

  writer.write_usize(tx.outputs.size());
  for (const auto& output : tx.outputs) {
    encode_tx_output(writer, output);
  }

  encode_option_hash(writer, tx.transaction_id);
  writer.write_varuint(tx.nonce);
  writer.write_varuint(tx.timestamp);
}

void encode_address_filter(BincodeWriter& writer, const AddressInclusionFilter& filter) {
  writer.write_usize(filter.bits.size());
  for (const auto b : filter.bits) {
    writer.write_u8(b);
  }
  writer.write_varuint(filter.num_bits);
  writer.write_varuint(filter.num_hashes);
}

void encode_block_meta(BincodeWriter& writer, const BlockMetadata& meta) {
  writer.write_fixed_array32(meta.block_pow_difficulty);
  writer.write_fixed_array32(meta.tx_pow_difficulty);
  encode_hash(writer, meta.previous_block);
  encode_option_hash(writer, meta.hash);
  writer.write_fixed_array32(meta.merkle_tree_root);
  encode_address_filter(writer, meta.address_inclusion_filter);
}

void encode_block(BincodeWriter& writer, const Block& block, size_t* nonce_offset) {
  writer.write_usize(block.transactions.size());
  for (const auto& tx : block.transactions) {
    encode_transaction(writer, tx);
  }

  writer.write_varuint(block.timestamp);

  if (nonce_offset != nullptr) {
    *nonce_offset = writer.buffer().size();
  }
  writer.write_varuint(block.nonce);

  encode_block_meta(writer, block.meta);
}

void encode_transaction_hashing(BincodeWriter& writer, const Transaction& tx, size_t* nonce_offset) {
  Transaction normalized = tx;
  normalized.transaction_id.reset();

  writer.write_usize(normalized.inputs.size());
  for (const auto& input : normalized.inputs) {
    encode_tx_input(writer, input);
  }

  writer.write_usize(normalized.outputs.size());
  for (const auto& output : normalized.outputs) {
    encode_tx_output(writer, output);
  }

  encode_option_hash(writer, normalized.transaction_id);

  if (nonce_offset != nullptr) {
    *nonce_offset = writer.buffer().size();
  }
  writer.write_varuint(normalized.nonce);
  writer.write_varuint(normalized.timestamp);
}

void hash_append(std::vector<uint8_t>& dest, const std::array<uint8_t, 32>& src) {
  dest.insert(dest.end(), src.begin(), src.end());
}

uint64_t little_endian_u64_prefix(const std::array<uint8_t, 32>& bytes) {
  uint64_t out = 0;
  for (size_t i = 0; i < 8; ++i) {
    out |= (static_cast<uint64_t>(bytes[i]) << (i * 8U));
  }
  return out;
}

uint64_t hash_filter_index(const std::array<uint8_t, 32>& address, uint32_t i) {
  std::array<uint8_t, 36> data{};
  std::copy(address.begin(), address.end(), data.begin());
  data[32] = static_cast<uint8_t>(i & 0xFFU);
  data[33] = static_cast<uint8_t>((i >> 8U) & 0xFFU);
  data[34] = static_cast<uint8_t>((i >> 16U) & 0xFFU);
  data[35] = static_cast<uint8_t>((i >> 24U) & 0xFFU);
  const auto digest = sha256(data.data(), data.size());
  return little_endian_u64_prefix(digest);
}

#ifdef SCRIG_HAVE_RANDOMX

struct RandomXState {
  randomx_flags flags = RANDOMX_FLAG_DEFAULT;
  randomx_cache* cache = nullptr;
  randomx_dataset* dataset = nullptr;
  bool full_mem = true;
  bool large_pages = false;
  bool initialized = false;
};

RandomXState g_randomx_state;
std::once_flag g_randomx_once;

bool hard_aes_runtime_probe_supported() {
#if defined(_WIN32) && defined(_MSC_VER) && \
  (defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86))
  return true;
#else
  return false;
#endif
}

bool hard_aes_runtime_probe() {
#if defined(_WIN32) && defined(_MSC_VER) && \
  (defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86))
  __try {
    __m128i state = _mm_setzero_si128();
    const __m128i key = _mm_set_epi32(0xA5A5A5A5, 0xC3C3C3C3, 0x5A5A5A5A, 0x3C3C3C3C);
    state = _mm_aesenc_si128(state, key);
    volatile int sink = _mm_cvtsi128_si32(state);
    (void)sink;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#else
  return false;
#endif
}

#if defined(_WIN32) && defined(_MSC_VER) && \
  (defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86))
bool windows_cpuid_reports_aes() {
  int info[4] = {0, 0, 0, 0};
  __cpuid(info, 1);
  return (info[2] & (1 << 25)) != 0;
}

struct WindowsCpuDiag {
  bool aes = false;
  bool avx2 = false;
  bool ssse3 = false;
  bool hypervisor_present = false;
  std::string vendor;
  std::string hypervisor_vendor;
};

WindowsCpuDiag windows_cpu_diag() {
  WindowsCpuDiag diag{};
  int info[4] = {0, 0, 0, 0};

  __cpuid(info, 0);
  const int max_leaf = info[0];
  char vendor[13] = {};
  std::memcpy(vendor + 0, &info[1], 4);
  std::memcpy(vendor + 4, &info[3], 4);
  std::memcpy(vendor + 8, &info[2], 4);
  vendor[12] = '\0';
  diag.vendor = vendor;

  if (max_leaf >= 1) {
    __cpuid(info, 1);
    diag.aes = (info[2] & (1 << 25)) != 0;
    diag.ssse3 = (info[2] & (1 << 9)) != 0;
    diag.hypervisor_present = (info[2] & (1u << 31)) != 0;
  }

  if (max_leaf >= 7) {
    __cpuidex(info, 7, 0);
    diag.avx2 = (info[1] & (1 << 5)) != 0;
  }

  if (diag.hypervisor_present) {
    __cpuid(info, 0x40000000);
    char hv_vendor[13] = {};
    std::memcpy(hv_vendor + 0, &info[1], 4);
    std::memcpy(hv_vendor + 4, &info[2], 4);
    std::memcpy(hv_vendor + 8, &info[3], 4);
    hv_vendor[12] = '\0';
    diag.hypervisor_vendor = hv_vendor;
  }

  return diag;
}
#endif

bool platform_supports_large_pages_hint() {
#if defined(__APPLE__)
  return false;
#else
  return true;
#endif
}

void release_randomx_state() {
  if (g_randomx_state.dataset != nullptr) {
    randomx_release_dataset(g_randomx_state.dataset);
    g_randomx_state.dataset = nullptr;
  }
  if (g_randomx_state.cache != nullptr) {
    randomx_release_cache(g_randomx_state.cache);
    g_randomx_state.cache = nullptr;
  }
  g_randomx_state.initialized = false;
}

bool try_init_randomx_once(randomx_flags flags, bool full_mem, uint32_t init_threads, std::string* error_reason) {
  release_randomx_state();

  auto* cache = randomx_alloc_cache(flags);
  if (cache == nullptr) {
    if (error_reason != nullptr) {
      *error_reason = "randomx_alloc_cache failed";
    }
    return false;
  }

  randomx_init_cache(cache, kRandomXSeed, static_cast<size_t>(kRandomXSeedLen));

  randomx_dataset* dataset = nullptr;
  if (full_mem) {
    dataset = randomx_alloc_dataset(flags);
    if (dataset == nullptr) {
      randomx_release_cache(cache);
      if (error_reason != nullptr) {
        *error_reason = "randomx_alloc_dataset failed";
      }
      return false;
    }

    const auto item_count = randomx_dataset_item_count();
    const uint32_t threads = std::max<uint32_t>(1U, init_threads);
    const uint64_t chunk = (item_count + threads - 1U) / threads;

    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (uint32_t i = 0; i < threads; ++i) {
      const uint64_t start = chunk * i;
      if (start >= item_count) {
        break;
      }
      const uint64_t count = std::min<uint64_t>(chunk, item_count - start);

      workers.emplace_back([dataset, cache, start, count]() {
        randomx_init_dataset(dataset, cache, start, count);
      });
    }

    for (auto& worker : workers) {
      worker.join();
    }
  }

  g_randomx_state.flags = flags;
  g_randomx_state.cache = cache;
  g_randomx_state.dataset = dataset;
  g_randomx_state.full_mem = full_mem;
  g_randomx_state.large_pages = (flags & RANDOMX_FLAG_LARGE_PAGES) != 0;
  g_randomx_state.initialized = true;
  return true;
}

void init_randomx_internal(const HashingConfig& config, uint32_t init_threads) {
  const randomx_flags supported_flags = randomx_get_flags();
  randomx_flags base_flags = RANDOMX_FLAG_DEFAULT;
  // Keep RandomX's preferred Argon2 implementation flags (AVX2/SSSE3) when available.
  base_flags = static_cast<randomx_flags>(base_flags | (supported_flags & RANDOMX_FLAG_ARGON2));

  if (config.jit) {
    if ((supported_flags & RANDOMX_FLAG_JIT) != 0) {
      base_flags = static_cast<randomx_flags>(base_flags | RANDOMX_FLAG_JIT);
    } else {
      std::cerr << "[RANDOMX] JIT requested but unavailable on this runtime/build. Continuing without JIT.\n";
    }
  }
  if (config.hard_aes) {
    const bool probe_supported = hard_aes_runtime_probe_supported();
    const bool probe_ok = probe_supported && hard_aes_runtime_probe();
    if ((supported_flags & RANDOMX_FLAG_HARD_AES) != 0) {
      base_flags = static_cast<randomx_flags>(base_flags | RANDOMX_FLAG_HARD_AES);
    } else if (probe_ok) {
      base_flags = static_cast<randomx_flags>(base_flags | RANDOMX_FLAG_HARD_AES);
      std::cerr << "[RANDOMX] HARD_AES not reported by CPUID probe; enabling after runtime AES instruction test.\n";
    } else {
#if defined(_WIN32) && defined(_MSC_VER) && \
  (defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86))
      const auto diag = windows_cpu_diag();
      std::cerr << "[RANDOMX] Windows AES diagnostics: cpuid_aes="
                << (diag.aes ? "1" : "0")
                << " ssse3=" << (diag.ssse3 ? "1" : "0")
                << " avx2=" << (diag.avx2 ? "1" : "0")
                << " hypervisor=" << (diag.hypervisor_present ? "1" : "0")
                << " vendor=" << diag.vendor;
      if (diag.hypervisor_present && !diag.hypervisor_vendor.empty()) {
        std::cerr << " hypervisor_vendor=" << diag.hypervisor_vendor;
      }
      std::cerr << " runtime_probe=" << (probe_ok ? "pass" : (probe_supported ? "fail" : "n/a")) << "\n";
#endif
      std::cerr << "[RANDOMX] HARD_AES requested but unavailable on this runtime/build. Continuing with soft AES.\n";
    }
  }
  if (config.secure) {
    base_flags = static_cast<randomx_flags>(base_flags | RANDOMX_FLAG_SECURE);
  }

  bool want_full_mem = config.full_mem;
  bool want_large_pages = config.huge_pages && platform_supports_large_pages_hint();

  auto make_flags = [&](randomx_flags base, bool full_mem, bool large_pages) {
    randomx_flags flags = base;
    if (full_mem) {
      flags = static_cast<randomx_flags>(flags | RANDOMX_FLAG_FULL_MEM);
    }
    if (large_pages) {
      flags = static_cast<randomx_flags>(flags | RANDOMX_FLAG_LARGE_PAGES);
    }
    return flags;
  };

  std::string reason;
  randomx_flags active_base_flags = base_flags;
  auto try_current = [&]() {
    return try_init_randomx_once(
      make_flags(active_base_flags, want_full_mem, want_large_pages),
      want_full_mem,
      init_threads,
      &reason);
  };

  if (try_current()) {
    return;
  }

  // Prefer preserving HARD_AES/JIT before disabling them; large page privilege failures
  // are common and should not force AES fallback.
  if (want_large_pages) {
    std::cerr << "[RANDOMX] Falling back: large pages unavailable (" << reason << "). Retrying without large pages.\n";
    want_large_pages = false;
    if (try_current()) {
      return;
    }
  }

  if ((active_base_flags & RANDOMX_FLAG_HARD_AES) != 0) {
    std::cerr << "[RANDOMX] Falling back: HARD_AES init failed (" << reason << "). Retrying with soft AES.\n";
    active_base_flags = static_cast<randomx_flags>(active_base_flags & ~RANDOMX_FLAG_HARD_AES);
    if (try_current()) {
      return;
    }
  }

  if ((active_base_flags & RANDOMX_FLAG_JIT) != 0) {
    std::cerr << "[RANDOMX] Falling back: JIT init failed (" << reason << "). Retrying without JIT.\n";
    active_base_flags = static_cast<randomx_flags>(active_base_flags & ~RANDOMX_FLAG_JIT);
    if ((active_base_flags & RANDOMX_FLAG_SECURE) != 0) {
      active_base_flags = static_cast<randomx_flags>(active_base_flags & ~RANDOMX_FLAG_SECURE);
    }
    if (try_current()) {
      return;
    }
  }

  if (want_full_mem) {
    std::cerr << "[RANDOMX] Falling back: full-memory mode unavailable (" << reason << "). Retrying in light mode.\n";
    want_full_mem = false;
    if (try_current()) {
      std::cerr << "[RANDOMX] Light mode enabled. Hashrate will be lower.\n";
      return;
    }
  }

  throw std::runtime_error("RandomX initialization failed: " + reason);
}

randomx_vm* get_thread_vm() {
  struct VmHolder {
    randomx_vm* vm = nullptr;
    ~VmHolder() {
      if (vm != nullptr) {
        randomx_destroy_vm(vm);
      }
    }
  };
  thread_local VmHolder holder;

  if (holder.vm == nullptr) {
    const randomx_cache* cache = g_randomx_state.cache;
    const randomx_dataset* dataset = g_randomx_state.full_mem ? g_randomx_state.dataset : nullptr;
    holder.vm = randomx_create_vm(
      g_randomx_state.flags,
      const_cast<randomx_cache*>(cache),
      const_cast<randomx_dataset*>(dataset));
    if (holder.vm == nullptr) {
      throw std::runtime_error("randomx_create_vm failed");
    }
  }

  return holder.vm;
}

#endif

#ifndef SCRIG_HAVE_RANDOMX
thread_local Hash g_pipeline_pending_hash = Hash::zero();
thread_local bool g_pipeline_has_pending = false;
#endif

} // namespace

void initialize_hashing(const HashingConfig& config, uint32_t init_threads) {
#ifdef SCRIG_HAVE_RANDOMX
  std::call_once(g_randomx_once, [&config, init_threads]() { init_randomx_internal(config, init_threads); });
#else
  (void)config;
  (void)init_threads;
#endif
}

void shutdown_hashing() {
#ifdef SCRIG_HAVE_RANDOMX
  release_randomx_state();
#endif
}

bool hashing_uses_randomx() {
#ifdef SCRIG_HAVE_RANDOMX
  return true;
#else
  return false;
#endif
}

HashingRuntimeProfile hashing_runtime_profile() {
  HashingRuntimeProfile profile{};
#ifdef SCRIG_HAVE_RANDOMX
  profile.randomx = true;
  profile.initialized = g_randomx_state.initialized;
  profile.full_mem = g_randomx_state.full_mem;
  profile.huge_pages = g_randomx_state.large_pages;
  profile.jit = (g_randomx_state.flags & RANDOMX_FLAG_JIT) != 0;
  profile.hard_aes = (g_randomx_state.flags & RANDOMX_FLAG_HARD_AES) != 0;
  profile.secure = (g_randomx_state.flags & RANDOMX_FLAG_SECURE) != 0;
#else
  profile.randomx = false;
  profile.initialized = true;
#endif
  return profile;
}

HashingCapabilityProfile hashing_capability_profile() {
  HashingCapabilityProfile profile{};
#ifdef SCRIG_HAVE_RANDOMX
  const randomx_flags flags = randomx_get_flags();
  profile.randomx = true;
  profile.jit = (flags & RANDOMX_FLAG_JIT) != 0;
  profile.hard_aes = (flags & RANDOMX_FLAG_HARD_AES) != 0;
  if (!profile.hard_aes && hard_aes_runtime_probe_supported() && hard_aes_runtime_probe()) {
    profile.hard_aes = true;
  }
  profile.secure = (flags & RANDOMX_FLAG_SECURE) != 0;
  profile.argon2_avx2 = (flags & RANDOMX_FLAG_ARGON2_AVX2) != 0;
  profile.argon2_ssse3 = (flags & RANDOMX_FLAG_ARGON2_SSSE3) != 0;
#endif
  return profile;
}

Hash hash_data(std::span<const uint8_t> data) {
#ifdef SCRIG_HAVE_RANDOMX
  if (!g_randomx_state.initialized) {
    throw std::runtime_error("hashing is not initialized");
  }

  Hash out = Hash::zero();
  auto* vm = get_thread_vm();
  randomx_calculate_hash(vm, data.data(), data.size(), out.bytes.data());
  return out;
#else
  return Hash{sha256(data.data(), data.size())};
#endif
}

Hash hash_data(const std::vector<uint8_t>& data) {
  return hash_data(std::span<const uint8_t>(data.data(), data.size()));
}

bool hashing_supports_pipeline() {
#ifdef SCRIG_HAVE_RANDOMX
  return true;
#else
  return false;
#endif
}

void hash_data_pipeline_begin(std::span<const uint8_t> data) {
#ifdef SCRIG_HAVE_RANDOMX
  if (!g_randomx_state.initialized) {
    throw std::runtime_error("hashing is not initialized");
  }
  auto* vm = get_thread_vm();
  randomx_calculate_hash_first(vm, data.data(), data.size());
#else
  g_pipeline_pending_hash = hash_data(data);
  g_pipeline_has_pending = true;
#endif
}

Hash hash_data_pipeline_next(std::span<const uint8_t> next_data) {
#ifdef SCRIG_HAVE_RANDOMX
  if (!g_randomx_state.initialized) {
    throw std::runtime_error("hashing is not initialized");
  }
  Hash out = Hash::zero();
  auto* vm = get_thread_vm();
  randomx_calculate_hash_next(vm, next_data.data(), next_data.size(), out.bytes.data());
  return out;
#else
  if (!g_pipeline_has_pending) {
    throw std::runtime_error("hashing pipeline has no pending input");
  }
  const Hash out = g_pipeline_pending_hash;
  g_pipeline_pending_hash = hash_data(next_data);
  g_pipeline_has_pending = true;
  return out;
#endif
}

Hash hash_data_pipeline_last() {
#ifdef SCRIG_HAVE_RANDOMX
  if (!g_randomx_state.initialized) {
    throw std::runtime_error("hashing is not initialized");
  }
  Hash out = Hash::zero();
  auto* vm = get_thread_vm();
  randomx_calculate_hash_last(vm, out.bytes.data());
  return out;
#else
  if (!g_pipeline_has_pending) {
    throw std::runtime_error("hashing pipeline has no pending input");
  }
  g_pipeline_has_pending = false;
  return g_pipeline_pending_hash;
#endif
}

DifficultyTarget calculate_block_difficulty_target(const DifficultyTarget& block_difficulty, size_t tx_count) {
  const double scale = (1.0 + SNAP_DIFFICULTY_DECAY_PER_TRANSACTION * static_cast<double>(tx_count)) * 1000.0;
  const uint64_t scaled = static_cast<uint64_t>(scale);
  bool overflowed = false;
  auto result = multiply_divide_target(block_difficulty, scaled, 1000U, &overflowed);
  if (overflowed) {
    result.fill(0xFFU);
  }
  return result;
}

bool hash_meets_target(const Hash& hash, const DifficultyTarget& target) {
  for (size_t i = 0; i < 32; ++i) {
    if (hash.bytes[i] < target[i]) {
      return true;
    }
    if (hash.bytes[i] > target[i]) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> transaction_pow_buffer(const Transaction& tx, size_t* nonce_offset) {
  BincodeWriter writer;
  encode_transaction_hashing(writer, tx, nonce_offset);
  return writer.buffer();
}

Hash compute_transaction_hash(const Transaction& tx) {
  return hash_data(transaction_pow_buffer(tx));
}

bool patch_nonce_in_pow_buffer(std::vector<uint8_t>& pow_buffer, size_t nonce_offset, uint64_t nonce) {
  if (nonce_offset >= pow_buffer.size()) {
    return false;
  }

  if (pow_buffer[nonce_offset] != 253U || nonce_offset + 9 > pow_buffer.size()) {
    return false;
  }

  patch_varuint_u64_in_place_9(pow_buffer, nonce_offset, nonce);
  return true;
}

std::vector<uint8_t> block_pow_buffer(const Block& block, size_t* nonce_offset) {
  Block hash_less = block;
  hash_less.meta.hash.reset();

  std::vector<uint8_t> tx_digest;
  tx_digest.reserve(hash_less.transactions.size() * 32);

  for (auto& tx : hash_less.transactions) {
    BincodeWriter tx_io_writer;
    tx_io_writer.write_usize(tx.inputs.size());
    for (const auto& input : tx.inputs) {
      encode_tx_input(tx_io_writer, input);
    }

    // Intentional compatibility with upstream logic: inputs are encoded twice.
    tx_io_writer.write_usize(tx.inputs.size());
    for (const auto& input : tx.inputs) {
      encode_tx_input(tx_io_writer, input);
    }

    const auto digest = sha256(tx_io_writer.buffer().data(), tx_io_writer.buffer().size());
    hash_append(tx_digest, digest);

    tx.inputs.clear();
    tx.outputs.clear();
  }

  BincodeWriter writer;
  encode_block(writer, hash_less, nonce_offset);

  if (block.timestamp > SNAP_SCIP1_MIGRATION) {
    auto& buf = writer.buffer();
    buf.insert(buf.end(), tx_digest.begin(), tx_digest.end());
  }

  return writer.buffer();
}

Hash compute_block_hash(const Block& block) {
  return hash_data(block_pow_buffer(block));
}

std::array<uint8_t, 32> build_merkle_root(const std::vector<Transaction>& txs) {
  if (txs.empty()) {
    return std::array<uint8_t, 32>{};
  }

  std::vector<std::array<uint8_t, 32>> leaves;
  leaves.reserve(txs.size());

  for (const auto& tx : txs) {
    if (!tx.transaction_id.has_value()) {
      leaves.push_back(std::array<uint8_t, 32>{});
      continue;
    }
    leaves.push_back(sha256(tx.transaction_id->bytes.data(), tx.transaction_id->bytes.size()));
  }

  while (leaves.size() > 1) {
    std::vector<std::array<uint8_t, 32>> next;
    next.reserve((leaves.size() + 1U) / 2U);

    for (size_t i = 0; i < leaves.size(); i += 2U) {
      const auto& left = leaves[i];
      const auto& right = (i + 1U < leaves.size()) ? leaves[i + 1U] : leaves[i];

      std::array<uint8_t, 64> pair{};
      std::copy(left.begin(), left.end(), pair.begin());
      std::copy(right.begin(), right.end(), pair.begin() + 32);
      next.push_back(sha256(pair.data(), pair.size()));
    }

    leaves = std::move(next);
  }

  return leaves.front();
}

AddressInclusionFilter build_address_filter(const std::vector<Transaction>& txs) {
  size_t address_count = 0;
  for (const auto& tx : txs) {
    address_count += tx.inputs.size();
    address_count += tx.outputs.size();
  }

  if (address_count == 0) {
    return AddressInclusionFilter{};
  }

  const double ln2 = std::log(2.0);
  const auto num_bits = static_cast<uint64_t>(
    std::ceil((-(static_cast<double>(address_count) * std::log(kAddressFilterFalsePositiveRate))) / (ln2 * ln2)));
  const auto num_hashes = static_cast<uint32_t>(
    std::ceil((static_cast<double>(num_bits) / static_cast<double>(address_count)) * ln2));

  AddressInclusionFilter filter;
  filter.bits.assign(static_cast<size_t>((num_bits + 7U) / 8U), 0);
  filter.num_bits = num_bits;
  filter.num_hashes = num_hashes;

  auto insert = [&](const std::array<uint8_t, 32>& address) {
    if (filter.num_bits == 0) {
      return;
    }
    for (uint32_t i = 0; i < filter.num_hashes; ++i) {
      const auto bit = hash_filter_index(address, i) % filter.num_bits;
      filter.bits[static_cast<size_t>(bit / 8U)] |= static_cast<uint8_t>(1U << (bit % 8U));
    }
  };

  for (const auto& tx : txs) {
    for (const auto& input : tx.inputs) {
      insert(input.output_owner.bytes);
    }
    for (const auto& output : tx.outputs) {
      insert(output.receiver.bytes);
    }
  }

  return filter;
}

bool trim_expired_transactions(Block& block, uint64_t now_ts) {
  const auto old_size = block.transactions.size();
  block.transactions.erase(
    std::remove_if(
      block.transactions.begin(),
      block.transactions.end(),
      [now_ts](const Transaction& tx) {
        return tx.timestamp + SNAP_EXPIRATION_TIME_SECONDS + 10ULL < now_ts;
      }),
    block.transactions.end());

  const bool removed = block.transactions.size() != old_size;
  if (removed) {
    block.meta.merkle_tree_root = build_merkle_root(block.transactions);
    block.meta.address_inclusion_filter = build_address_filter(block.transactions);
  }
  return removed;
}

Transaction build_reward_transaction(
  const PublicKey& miner,
  uint64_t reward,
  uint64_t timestamp) {

  Transaction tx;
  tx.inputs.clear();
  tx.outputs = {
    TransactionOutput{calculate_dev_fee(reward), dev_wallet()},
    TransactionOutput{reward - calculate_dev_fee(reward), miner},
  };
  tx.transaction_id.reset();
  tx.nonce = 0x100000000ULL;
  tx.timestamp = timestamp;

  return tx;
}

} // namespace scrig
