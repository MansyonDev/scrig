#include "scrig/node_client.hpp"

#include "scrig/json.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace scrig {

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

std::mutex g_active_sockets_mutex;
std::unordered_set<SocketHandle> g_active_sockets;

#ifdef _WIN32
void initialize_winsock_once() {
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
#endif

void interrupt_socket(SocketHandle sock) {
  if (sock == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  (void)shutdown(sock, SD_BOTH);
#else
  (void)shutdown(sock, SHUT_RDWR);
#endif
}

void close_socket(SocketHandle sock) {
  if (sock == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  closesocket(sock);
#else
  close(sock);
#endif
}

void write_all(SocketHandle sock, const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
#ifdef _WIN32
    const int n = send(sock, reinterpret_cast<const char*>(data + sent), static_cast<int>(len - sent), 0);
#else
    const ssize_t n = send(sock, data + sent, len - sent, 0);
#endif
    if (n <= 0) {
      throw std::runtime_error("socket send failed");
    }
    sent += static_cast<size_t>(n);
  }
}

void read_all(SocketHandle sock, uint8_t* data, size_t len) {
  size_t received = 0;
  while (received < len) {
#ifdef _WIN32
    const int n = recv(sock, reinterpret_cast<char*>(data + received), static_cast<int>(len - received), 0);
#else
    const ssize_t n = recv(sock, data + received, len - received, 0);
#endif
    if (n <= 0) {
      throw std::runtime_error("socket recv failed");
    }
    received += static_cast<size_t>(n);
  }
}

uint32_t read_u32_be(const std::array<uint8_t, 4>& bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24U) |
         (static_cast<uint32_t>(bytes[1]) << 16U) |
         (static_cast<uint32_t>(bytes[2]) << 8U) |
         static_cast<uint32_t>(bytes[3]);
}

std::array<uint8_t, 4> write_u32_be(uint32_t value) {
  return {
    static_cast<uint8_t>((value >> 24U) & 0xFFU),
    static_cast<uint8_t>((value >> 16U) & 0xFFU),
    static_cast<uint8_t>((value >> 8U) & 0xFFU),
    static_cast<uint8_t>(value & 0xFFU),
  };
}

const JsonValue::object& expect_single_variant_object(const JsonValue& value) {
  if (!value.is_object()) {
    throw std::runtime_error("response is not an object");
  }
  return value.as_object();
}

const JsonValue& get_variant_payload(const JsonValue& response, const std::string& variant) {
  const auto& obj = expect_single_variant_object(response);
  const auto it = obj.find(variant);
  if (it == obj.end()) {
    throw std::runtime_error("unexpected response variant");
  }
  return it->second;
}

} // namespace

NodeClient::NodeClient(std::string host, uint16_t port)
  : host_(std::move(host)), port_(port) {}

NodeClient::~NodeClient() {
  disconnect();
}

void NodeClient::interrupt_all() {
  std::vector<SocketHandle> sockets;
  {
    std::lock_guard<std::mutex> lock(g_active_sockets_mutex);
    sockets.reserve(g_active_sockets.size());
    for (const auto sock : g_active_sockets) {
      sockets.push_back(sock);
    }
  }
  for (const auto sock : sockets) {
    interrupt_socket(sock);
  }
}

void NodeClient::connect() {
  std::lock_guard<std::mutex> lock(io_mutex_);
  if (static_cast<SocketHandle>(socket_fd_) != kInvalidSocket) {
    return;
  }
  open_socket_locked();
}

void NodeClient::disconnect() {
  std::lock_guard<std::mutex> lock(io_mutex_);
  close_socket_locked();
}

void NodeClient::open_socket_locked() {
#ifdef _WIN32
  initialize_winsock_once();
#endif

  const std::string port_str = std::to_string(port_);

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* result = nullptr;
  if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result) != 0) {
    throw std::runtime_error("failed to resolve node host");
  }

  SocketHandle sock = kInvalidSocket;
  for (auto* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
    sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (sock == kInvalidSocket) {
      continue;
    }

#ifdef _WIN32
    const int addr_len = static_cast<int>(ptr->ai_addrlen);
#else
    const socklen_t addr_len = static_cast<socklen_t>(ptr->ai_addrlen);
#endif

    if (::connect(sock, ptr->ai_addr, addr_len) == 0) {
      break;
    }

    close_socket(sock);
    sock = kInvalidSocket;
  }

  freeaddrinfo(result);

  if (sock == kInvalidSocket) {
    throw std::runtime_error("failed to connect to node");
  }

  socket_fd_ = static_cast<std::intptr_t>(sock);
  {
    std::lock_guard<std::mutex> sockets_lock(g_active_sockets_mutex);
    g_active_sockets.insert(sock);
  }
}

void NodeClient::close_socket_locked() {
  const auto sock = static_cast<SocketHandle>(socket_fd_);
  if (sock != kInvalidSocket) {
    std::lock_guard<std::mutex> sockets_lock(g_active_sockets_mutex);
    g_active_sockets.erase(sock);
  }
  close_socket(sock);
  socket_fd_ = static_cast<std::intptr_t>(kInvalidSocket);
  subscribed_ = false;
  pool_handshake_done_ = false;
}

JsonValue NodeClient::decode_next_message() {
  std::array<uint8_t, 4> response_len_buf{};
  read_all(static_cast<SocketHandle>(socket_fd_), response_len_buf.data(), response_len_buf.size());
  const uint32_t response_len = read_u32_be(response_len_buf);

  std::vector<uint8_t> response_buf(response_len);
  if (response_len > 0) {
    read_all(static_cast<SocketHandle>(socket_fd_), response_buf.data(), response_buf.size());
  }

  const std::string response_text(response_buf.begin(), response_buf.end());
  return parse_json(response_text);
}

JsonValue NodeClient::send_request(const JsonValue& request) {
  std::lock_guard<std::mutex> lock(io_mutex_);

  if (static_cast<SocketHandle>(socket_fd_) == kInvalidSocket) {
    open_socket_locked();
  }

  const std::string payload = to_json(request, false);
  const auto len = write_u32_be(static_cast<uint32_t>(payload.size()));

  try {
    write_all(static_cast<SocketHandle>(socket_fd_), len.data(), len.size());
    write_all(static_cast<SocketHandle>(socket_fd_), reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    return decode_next_message();
  } catch (...) {
    close_socket_locked();
    throw;
  }
}

DifficultyTarget NodeClient::initialize_pool_handshake(const PublicKey& miner_public) {
  std::lock_guard<std::mutex> lock(io_mutex_);

  if (static_cast<SocketHandle>(socket_fd_) == kInvalidSocket) {
    open_socket_locked();
  }

  write_all(static_cast<SocketHandle>(socket_fd_), miner_public.bytes.data(), miner_public.bytes.size());

  DifficultyTarget difficulty{};
  read_all(static_cast<SocketHandle>(socket_fd_), difficulty.data(), difficulty.size());

  pool_handshake_done_ = true;
  return difficulty;
}

uint64_t NodeClient::height() {
  const auto response = send_request(JsonValue("Height"));
  const auto& payload = get_variant_payload(response, "Height").as_object();
  const auto it = payload.find("height");
  if (it == payload.end()) {
    throw std::runtime_error("missing response Height.height");
  }
  return it->second.as_uint64();
}

DifficultyInfo NodeClient::difficulty() {
  const auto response = send_request(JsonValue("Difficulty"));
  const auto& payload = get_variant_payload(response, "Difficulty").as_object();

  DifficultyInfo info;

  auto tx_it = payload.find("transaction_difficulty");
  auto block_it = payload.find("block_difficulty");
  if (tx_it == payload.end() || block_it == payload.end()) {
    throw std::runtime_error("missing response difficulty fields");
  }

  info.transaction_difficulty = difficulty_target_from_json(tx_it->second);
  info.block_difficulty = difficulty_target_from_json(block_it->second);
  return info;
}

uint64_t NodeClient::reward() {
  const auto response = send_request(JsonValue("Reward"));
  const auto& payload = get_variant_payload(response, "Reward").as_object();
  const auto it = payload.find("reward");
  if (it == payload.end()) {
    throw std::runtime_error("missing response Reward.reward");
  }
  return it->second.as_uint64();
}

std::optional<Hash> NodeClient::block_hash(uint64_t height_value) {
  JsonValue::object request_payload{{"height", JsonValue(height_value)}};
  JsonValue::object request{{"BlockHash", JsonValue(std::move(request_payload))}};

  const auto response = send_request(JsonValue(std::move(request)));
  const auto& payload = get_variant_payload(response, "BlockHash").as_object();
  const auto it = payload.find("hash");
  if (it == payload.end() || it->second.is_null()) {
    return std::nullopt;
  }

  return hash_from_json(it->second);
}

std::optional<Block> NodeClient::block_by_hash(const Hash& hash) {
  JsonValue::object request_payload{{"block_hash", hash_to_json(hash)}};
  JsonValue::object request{{"Block", JsonValue(std::move(request_payload))}};

  const auto response = send_request(JsonValue(std::move(request)));
  const auto& payload = get_variant_payload(response, "Block").as_object();
  const auto it = payload.find("block");
  if (it == payload.end() || it->second.is_null()) {
    return std::nullopt;
  }

  return block_from_json(it->second);
}

MempoolPage NodeClient::mempool_page(uint32_t page) {
  JsonValue::object request_payload{{"page", JsonValue(static_cast<uint64_t>(page))}};
  JsonValue::object request{{"Mempool", JsonValue(std::move(request_payload))}};

  const auto response = send_request(JsonValue(std::move(request)));
  const auto& payload = get_variant_payload(response, "Mempool").as_object();

  MempoolPage out;

  const auto mempool_it = payload.find("mempool");
  if (mempool_it == payload.end()) {
    throw std::runtime_error("missing response Mempool.mempool");
  }

  for (const auto& tx : mempool_it->second.as_array()) {
    out.mempool.push_back(transaction_from_json(tx));
  }

  const auto next_it = payload.find("next_page");
  if (next_it != payload.end() && !next_it->second.is_null()) {
    out.next_page = static_cast<uint32_t>(next_it->second.as_uint64());
  }

  return out;
}

std::vector<Transaction> NodeClient::mempool_all(uint32_t max_pages) {
  std::vector<Transaction> all;
  uint32_t page = 0;

  while (true) {
    const auto result = mempool_page(page);
    all.insert(all.end(), result.mempool.begin(), result.mempool.end());

    if (!result.next_page.has_value() || page >= max_pages) {
      break;
    }

    page = *result.next_page;
  }

  return all;
}

DifficultyTarget NodeClient::live_transaction_difficulty() {
  const auto response = send_request(JsonValue("LiveTransactionDifficulty"));
  const auto& payload = get_variant_payload(response, "LiveTransactionDifficulty").as_object();

  const auto it = payload.find("live_difficulty");
  if (it == payload.end()) {
    throw std::runtime_error("missing response LiveTransactionDifficulty.live_difficulty");
  }

  return difficulty_target_from_json(it->second);
}

bool NodeClient::submit_block(const Block& block) {
  JsonValue::object request_payload{{"new_block", block_to_json(block)}};
  JsonValue::object request{{"NewBlock", JsonValue(std::move(request_payload))}};

  const auto response = send_request(JsonValue(std::move(request)));
  const auto& payload = get_variant_payload(response, "NewBlock").as_object();

  const auto it = payload.find("status");
  if (it == payload.end()) {
    throw std::runtime_error("missing response NewBlock.status");
  }

  if (it->second.is_string()) {
    return it->second.as_string() == "Ok";
  }

  if (it->second.is_object()) {
    const auto& status_obj = it->second.as_object();
    if (status_obj.find("Ok") != status_obj.end()) {
      return true;
    }
    if (status_obj.find("Err") != status_obj.end()) {
      return false;
    }
  }

  return false;
}

void NodeClient::subscribe_chain_events() {
  std::lock_guard<std::mutex> lock(io_mutex_);

  if (static_cast<SocketHandle>(socket_fd_) == kInvalidSocket) {
    open_socket_locked();
  }

  const std::string payload = to_json(JsonValue("SubscribeToChainEvents"), false);
  const auto len = write_u32_be(static_cast<uint32_t>(payload.size()));

  try {
    write_all(static_cast<SocketHandle>(socket_fd_), len.data(), len.size());
    write_all(static_cast<SocketHandle>(socket_fd_), reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    subscribed_ = true;
  } catch (...) {
    close_socket_locked();
    throw;
  }
}

ChainEvent NodeClient::wait_chain_event() {
  std::lock_guard<std::mutex> lock(io_mutex_);

  if (!subscribed_) {
    throw std::runtime_error("chain event stream not subscribed");
  }

  const auto response = decode_next_message();
  const auto& payload = get_variant_payload(response, "ChainEvent").as_object();

  const auto it = payload.find("event");
  if (it == payload.end()) {
    throw std::runtime_error("missing response ChainEvent.event");
  }

  return chain_event_from_json(it->second);
}

} // namespace scrig
