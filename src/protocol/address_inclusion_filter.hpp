#pragma once
#include <vector>
#include <cstdint>
#include "codec/bincode2.hpp"

namespace scrig::protocol {

struct AddressInclusionFilter {
    std::vector<std::uint8_t> bits;
    std::size_t num_bits;
    std::uint32_t num_hashes;

    void bincode2_encode(scrig::codec::Bincode2Writer& w) const {
        w.vec(bits);
        w.usize(num_bits);
        w.u32_le(num_hashes);
    }
};

}