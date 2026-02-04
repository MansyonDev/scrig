#include "hashing_buffer.hpp"
#include "bincode_encode.hpp"
#include <openssl/sha.h>
#include "codec/bincode2.hpp"

namespace scrig::protocol {

static std::vector<std::uint8_t> sha256(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out(32);
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    if (!data.empty()) SHA256_Update(&ctx, data.data(), data.size());
    SHA256_Final(out.data(), &ctx);
    return out;
}

std::vector<std::uint8_t> get_block_hashing_buffer(const Block& b) {
    Block hash_less = b;
    hash_less.meta.hash.reset();

    std::vector<std::uint8_t> tx_digest;
    tx_digest.reserve(hash_less.transactions.size() * 32);

    for (auto& tx : hash_less.transactions) {
        scrig::codec::Bincode2Writer w;
        w.vec(tx.inputs);
        w.vec(tx.inputs);
        auto digest = sha256(w.out);
        tx_digest.insert(tx_digest.end(), digest.begin(), digest.end());
        tx.inputs.clear();
        tx.outputs.clear();
    }

    scrig::codec::Bincode2Writer block_writer;
    block_writer.encode(hash_less);

    if (b.timestamp > SCIP_1_MIGRATION) {
        block_writer.out.insert(
            block_writer.out.end(),
            tx_digest.begin(),
            tx_digest.end()
        );
    }

    return block_writer.out;
}

}