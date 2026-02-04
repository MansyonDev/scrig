#include <iostream>
#include <iomanip>
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

    std::cout << "C++ hashing buffer (" << buf.size() << " bytes):\n";
    for (size_t i = 0; i < buf.size(); i++) {
        std::cout
            << std::hex
            << std::setw(2)
            << std::setfill('0')
            << (int)buf[i]
            << " ";
    }
    std::cout << std::dec << "\n";

    return 0;
}