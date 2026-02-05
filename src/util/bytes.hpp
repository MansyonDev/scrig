#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <string>

namespace scrig::util {

std::array<std::uint8_t,4> u32be(std::uint32_t v);
std::uint32_t read_u32be(const std::uint8_t* p);
std::string hex(const std::vector<std::uint8_t>& v);

}