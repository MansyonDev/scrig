#include "bytes.hpp"
#include <sstream>
#include <iomanip>

namespace scrig::util {

std::array<std::uint8_t,4> u32be(std::uint32_t v) {
  return { (std::uint8_t)((v>>24)&0xFF), (std::uint8_t)((v>>16)&0xFF), (std::uint8_t)((v>>8)&0xFF), (std::uint8_t)(v&0xFF) };
}

std::uint32_t read_u32be(const std::uint8_t* p) {
  return (std::uint32_t(p[0])<<24) | (std::uint32_t(p[1])<<16) | (std::uint32_t(p[2])<<8) | std::uint32_t(p[3]);
}

std::string hex(const std::vector<std::uint8_t>& v) {
  std::ostringstream ss;
  for(auto b: v) ss<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)b;
  return ss.str();
}

}