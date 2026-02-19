#include "scrig/net_platform.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace scrig {

SocketHandle invalid_socket_handle() {
  return static_cast<SocketHandle>(-1);
}

void initialize_network_stack_once() {
}

SocketHandle connect_tcp_socket(const std::string& host, uint16_t port) {
  const std::string port_str = std::to_string(port);

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
    throw std::runtime_error("failed to resolve node host");
  }

  int sock = -1;
  for (auto* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
    sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (sock < 0) {
      continue;
    }

    const socklen_t addr_len = static_cast<socklen_t>(ptr->ai_addrlen);
    if (::connect(sock, ptr->ai_addr, addr_len) == 0) {
      break;
    }

    close(sock);
    sock = -1;
  }

  freeaddrinfo(result);

  if (sock < 0) {
    throw std::runtime_error("failed to connect to node");
  }

  return static_cast<SocketHandle>(sock);
}

void interrupt_socket_handle(SocketHandle sock) {
  if (sock == invalid_socket_handle()) {
    return;
  }
  (void)shutdown(static_cast<int>(sock), SHUT_RDWR);
}

void close_socket_handle(SocketHandle sock) {
  if (sock == invalid_socket_handle()) {
    return;
  }
  (void)close(static_cast<int>(sock));
}

size_t send_socket_data(SocketHandle sock, const uint8_t* data, size_t len) {
  const ssize_t n = send(static_cast<int>(sock), data, len, 0);
  if (n <= 0) {
    return 0;
  }
  return static_cast<size_t>(n);
}

size_t recv_socket_data(SocketHandle sock, uint8_t* data, size_t len) {
  const ssize_t n = recv(static_cast<int>(sock), data, len, 0);
  if (n <= 0) {
    return 0;
  }
  return static_cast<size_t>(n);
}

} // namespace scrig
