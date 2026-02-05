#include "socket.hpp"
#include <stdexcept>
#include <cstring>

#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
static void wsa_init_once() {
  static bool inited=false;
  if(inited) return;
  WSADATA w;
  if(WSAStartup(MAKEWORD(2,2), &w)!=0) throw std::runtime_error("WSAStartup failed");
  inited=true;
}
#else
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace scrig::net {

struct SockHandle {
#if defined(_WIN32)
  SOCKET s = INVALID_SOCKET;
#else
  int s = -1;
#endif
};

static void close_sock(SockHandle* h) {
  if(!h) return;
#if defined(_WIN32)
  if(h->s!=INVALID_SOCKET) { closesocket(h->s); h->s=INVALID_SOCKET; }
#else
  if(h->s>=0) { close(h->s); h->s=-1; }
#endif
}

Socket::Socket() : h(new SockHandle()) {}
Socket::~Socket() { close_sock((SockHandle*)h); delete (SockHandle*)h; }
Socket::Socket(Socket&& o) noexcept : h(o.h) { o.h=nullptr; }
Socket& Socket::operator=(Socket&& o) noexcept { if(this!=&o){ this->~Socket(); h=o.h; o.h=nullptr; } return *this; }

void Socket::connect_tcp(const std::string& host, std::uint16_t port) {
#if defined(_WIN32)
  wsa_init_once();
#endif
  auto sh = (SockHandle*)h;
  close_sock(sh);

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* res=nullptr;
  auto port_str = std::to_string(port);
  if(getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res)!=0) throw std::runtime_error("getaddrinfo failed");

  addrinfo* p=res;
  for(; p; p=p->ai_next) {
#if defined(_WIN32)
    SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if(s==INVALID_SOCKET) continue;
    if(::connect(s, p->ai_addr, (int)p->ai_addrlen)==0) { sh->s=s; break; }
    closesocket(s);
#else
    int s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if(s<0) continue;
    if(::connect(s, p->ai_addr, p->ai_addrlen)==0) { sh->s=s; break; }
    close(s);
#endif
  }

  freeaddrinfo(res);

#if defined(_WIN32)
  if(sh->s==INVALID_SOCKET) throw std::runtime_error("connect failed");
#else
  if(sh->s<0) throw std::runtime_error("connect failed");
#endif
}

void Socket::set_nodelay(bool on) {
  auto sh = (SockHandle*)h;
  int v = on ? 1 : 0;
#if defined(_WIN32)
  if(setsockopt(sh->s, IPPROTO_TCP, TCP_NODELAY, (const char*)&v, sizeof(v))!=0) throw std::runtime_error("setsockopt nodelay failed");
#else
  if(setsockopt(sh->s, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v))!=0) throw std::runtime_error("setsockopt nodelay failed");
#endif
}

void Socket::write_all(const std::uint8_t* data, std::size_t n) {
  auto sh = (SockHandle*)h;
  std::size_t off=0;
  while(off<n) {
#if defined(_WIN32)
    int sent = send(sh->s, (const char*)data+off, (int)(n-off), 0);
    if(sent<=0) throw std::runtime_error("send failed");
#else
    ssize_t sent = send(sh->s, data+off, n-off, 0);
    if(sent<=0) throw std::runtime_error("send failed");
#endif
    off += (std::size_t)sent;
  }
}

void Socket::read_exact(std::uint8_t* out, std::size_t n) {
  auto sh = (SockHandle*)h;
  std::size_t off=0;
  while(off<n) {
#if defined(_WIN32)
    int got = recv(sh->s, (char*)out+off, (int)(n-off), 0);
    if(got<=0) throw std::runtime_error("recv failed");
#else
    ssize_t got = recv(sh->s, out+off, n-off, 0);
    if(got<=0) throw std::runtime_error("recv failed");
#endif
    off += (std::size_t)got;
  }
}

}