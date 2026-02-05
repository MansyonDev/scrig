#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "socket.hpp"

namespace scrig::net {

struct FramedTcp {
  Socket sock;

  void connect(const std::string& host, std::uint16_t port);
  void send_frame(const std::vector<std::uint8_t>& payload);
  std::vector<std::uint8_t> recv_frame();
};

}