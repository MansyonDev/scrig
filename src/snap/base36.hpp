#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace scrig::snap::base36 {
std::optional<std::array<uint8_t, 32>> decode_32(const std::string& s);
std::string encode_32(const std::array<uint8_t, 32>& bytes);
}