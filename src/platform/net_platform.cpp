#include "scrig/net_platform.hpp"

#include <stdexcept>

namespace scrig {

SocketHandle invalid_socket_handle() {
  return static_cast<SocketHandle>(-1);
}

void initialize_network_stack_once() {
}

SocketHandle connect_tcp_socket(const std::string&, uint16_t) {
  throw std::runtime_error("network stack is unavailable on this platform build");
}

void interrupt_socket_handle(SocketHandle) {
}

void close_socket_handle(SocketHandle) {
}

size_t send_socket_data(SocketHandle, const uint8_t*, size_t) {
  return 0;
}

size_t recv_socket_data(SocketHandle, uint8_t*, size_t) {
  return 0;
}

} // namespace scrig
