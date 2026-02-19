#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace scrig {

using SocketHandle = std::intptr_t;

SocketHandle invalid_socket_handle();
void initialize_network_stack_once();
SocketHandle connect_tcp_socket(const std::string& host, uint16_t port);
void interrupt_socket_handle(SocketHandle sock);
void close_socket_handle(SocketHandle sock);
size_t send_socket_data(SocketHandle sock, const uint8_t* data, size_t len);
size_t recv_socket_data(SocketHandle sock, uint8_t* data, size_t len);

} // namespace scrig
