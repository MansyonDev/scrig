#pragma once
#include <vector>
#include <cstdint>
#include "block.hpp"
#include "codec/bincode2.hpp"

namespace scrig::protocol {

static constexpr std::uint64_t SCIP_1_MIGRATION = 1760140800ULL;

std::vector<std::uint8_t> get_block_hashing_buffer(const Block& b);

}