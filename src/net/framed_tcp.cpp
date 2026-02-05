#include "framed_tcp.hpp"
#include "../util/bytes.hpp"

namespace scrig::net {

void FramedTcp::connect(const std::string& host, std::uint16_t port) {
  sock.connect_tcp(host, port);
  sock.set_nodelay(true);
}

void FramedTcp::send_frame(const std::vector<std::uint8_t>& payload) {
  auto len = scrig::util::u32be((std::uint32_t)payload.size());
  sock.write_all(len.data(), 4);
  if(!payload.empty()) sock.write_all(payload.data(), payload.size());
}

std::vector<std::uint8_t> FramedTcp::recv_frame() {
  std::uint8_t hdr[4];
  sock.read_exact(hdr, 4);
  auto n = scrig::util::read_u32be(hdr);
  std::vector<std::uint8_t> buf(n);
  if(n) sock.read_exact(buf.data(), n);
  return buf;
}

}