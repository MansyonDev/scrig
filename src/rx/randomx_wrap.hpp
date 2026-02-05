#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace scrig::rx {

class RandomX {
public:
    static void init(bool full_mode);
    static std::array<uint8_t, 32> hash(std::span<const uint8_t> data);
};

}