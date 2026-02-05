#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace scrig::net {

struct Socket {
  Socket();
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&&) noexcept;
  Socket& operator=(Socket&&) noexcept;

  void connect_tcp(const std::string& host, std::uint16_t port);
  void set_nodelay(bool on);
  void write_all(const std::uint8_t* data, std::size_t n);
  void read_exact(std::uint8_t* out, std::size_t n);

private:
  void* h;
};

}