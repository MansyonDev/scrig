#pragma once
#include <vector>
#include <optional>
#include <cstdint>
#include "codec/bincode2.hpp"
#include "hash.hpp"
#include "keys.hpp"

namespace scrig::protocol {

struct Signature64 {
    std::array<std::uint8_t, 64> bytes{};
    void bincode2_encode(scrig::codec::Bincode2Writer& w) const {
        w.out.insert(w.out.end(), bytes.begin(), bytes.end());
    }
};

struct TransactionInput {
    Hash32 transaction_id;
    std::size_t output_index;
    std::optional<Signature64> signature;
    Public32 output_owner;

    void bincode2_encode(scrig::codec::Bincode2Writer& w) const {
        w.encode(transaction_id);
        w.usize(output_index);
        w.option(signature);
        w.encode(output_owner);
    }
};

struct TransactionOutput {
    std::uint64_t amount;
    Public32 receiver;

    void bincode2_encode(scrig::codec::Bincode2Writer& w) const {
        w.u64(amount);
        w.encode(receiver);
    }
};

struct Transaction {
    std::vector<TransactionInput> inputs;
    std::vector<TransactionOutput> outputs;
    std::optional<Hash32> transaction_id;
    std::uint64_t nonce;
    std::uint64_t timestamp;

    void bincode2_encode(scrig::codec::Bincode2Writer& w) const {
        w.vec(inputs);
        w.vec(outputs);
        w.option(transaction_id);
        w.u64(nonce);
        w.u64(timestamp);
    }
};

}