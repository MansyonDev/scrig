#pragma once
#include <vector>
#include <optional>
#include <array>
#include <cstdint>
#include "codec/bincode2.hpp"
#include "transaction.hpp"
#include "address_inclusion_filter.hpp"
#include "hash.hpp"

namespace scrig::protocol {

struct BlockMetadata {
    std::array<std::uint8_t, 32> block_pow_difficulty;
    std::array<std::uint8_t, 32> tx_pow_difficulty;
    Hash32 previous_block;
    std::optional<Hash32> hash;
    std::array<std::uint8_t, 32> merkle_tree_root;
    AddressInclusionFilter address_inclusion_filter;

    void bincode2_encode(scrig::codec::Bincode2Writer& w) const {
        w.out.insert(w.out.end(), block_pow_difficulty.begin(), block_pow_difficulty.end());
        w.out.insert(w.out.end(), tx_pow_difficulty.begin(), tx_pow_difficulty.end());
        w.encode(previous_block);
        w.option(hash);
        w.out.insert(w.out.end(), merkle_tree_root.begin(), merkle_tree_root.end());
        w.encode(address_inclusion_filter);
    }
};

struct Block {
    std::vector<Transaction> transactions;
    std::uint64_t timestamp;
    std::uint64_t nonce;
    BlockMetadata meta;

    void bincode2_encode(scrig::codec::Bincode2Writer& w) const {
        w.vec(transactions);
        w.u64(timestamp);
        w.u64(nonce);
        w.encode(meta);
    }
};

}