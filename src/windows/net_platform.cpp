#include "scrig/net_platform.hpp"

#include <mutex>
#include <stdexcept>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace scrig {

SocketHandle invalid_socket_handle() {
  return static_cast<SocketHandle>(INVALID_SOCKET);
}

void initialize_network_stack_once() {
  static bool initialized = false;
  static std::mutex lock;

  std::lock_guard<std::mutex> guard(lock);
  if (initialized) {
    return;
  }

  WSADATA wsa_data{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    throw std::runtime_error("WSAStartup failed");
  }

  initialized = true;
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

  SOCKET sock = INVALID_SOCKET;
  for (auto* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
    sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (sock == INVALID_SOCKET) {
      continue;
    }

    const int addr_len = static_cast<int>(ptr->ai_addrlen);
    if (::connect(sock, ptr->ai_addr, addr_len) == 0) {
      break;
    }

    closesocket(sock);
    sock = INVALID_SOCKET;
  }

  freeaddrinfo(result);

  if (sock == INVALID_SOCKET) {
    throw std::runtime_error("failed to connect to node");
  }

  return static_cast<SocketHandle>(sock);
}

void interrupt_socket_handle(SocketHandle sock) {
  if (sock == invalid_socket_handle()) {
    return;
  }
  (void)shutdown(static_cast<SOCKET>(sock), SD_BOTH);
}

void close_socket_handle(SocketHandle sock) {
  if (sock == invalid_socket_handle()) {
    return;
  }
  closesocket(static_cast<SOCKET>(sock));
}

size_t send_socket_data(SocketHandle sock, const uint8_t* data, size_t len) {
  const int n = send(
    static_cast<SOCKET>(sock),
    reinterpret_cast<const char*>(data),
    static_cast<int>(len),
    0);
  if (n <= 0) {
    return 0;
  }
  return static_cast<size_t>(n);
}

size_t recv_socket_data(SocketHandle sock, uint8_t* data, size_t len) {
  const int n = recv(
    static_cast<SOCKET>(sock),
    reinterpret_cast<char*>(data),
    static_cast<int>(len),
    0);
  if (n <= 0) {
    return 0;
  }
  return static_cast<size_t>(n);
}

} // namespace scrig
