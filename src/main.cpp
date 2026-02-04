#include <iostream>
#include "protocol/hashing_buffer.hpp"

int main() {
    using namespace scrig::protocol;

    Block block{};
    block.timestamp = 0;
    block.nonce = 0;

    block.meta.block_pow_difficulty.fill(0);
    block.meta.tx_pow_difficulty.fill(0);
    block.meta.previous_block.bytes.fill(0);
    block.meta.merkle_tree_root.fill(0);

    block.meta.address_inclusion_filter.bits.clear();
    block.meta.address_inclusion_filter.num_bits = 0;
    block.meta.address_inclusion_filter.num_hashes = 0;

    auto buf = get_block_hashing_buffer(block);

    std::cout << "hashing buffer size: " << buf.size() << std::endl;

    return 0;
}
