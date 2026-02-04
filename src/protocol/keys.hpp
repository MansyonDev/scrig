#pragma once
#include <array>
#include <cstdint>
#include "codec/bincode2.hpp"

namespace scrig::protocol {

struct Public32 {
    std::array<std::uint8_t, 32> bytes{};
    void bincode2_encode(scrig::codec::Bincode2Writer& w) const {
        w.out.insert(w.out.end(), bytes.begin(), bytes.end());
    }
};

}