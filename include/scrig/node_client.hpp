#pragma once

#include "scrig/json.hpp"
#include "scrig/types.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scrig {

class NodeClient {
public:
  NodeClient(std::string host, uint16_t port);
  ~NodeClient();

  NodeClient(const NodeClient&) = delete;
  NodeClient& operator=(const NodeClient&) = delete;

  static void interrupt_all();

  void connect();
  void disconnect();

  DifficultyTarget initialize_pool_handshake(const PublicKey& miner_public);

  uint64_t height();
  uint64_t balance(const PublicKey& address);
  DifficultyInfo difficulty();
  uint64_t reward();
  std::optional<Hash> block_hash(uint64_t height);
  std::optional<Block> block_by_hash(const Hash& hash);
  MempoolPage mempool_page(uint32_t page);
  std::vector<Transaction> mempool_all(uint32_t max_pages = 32);

  DifficultyTarget live_transaction_difficulty();

  bool submit_block(const Block& block);

  void subscribe_chain_events();
  ChainEvent wait_chain_event();

private:
  JsonValue send_request(const JsonValue& request);
  JsonValue decode_next_message();
  DifficultyTarget initialize_pool_handshake_locked(const PublicKey& miner_public);

  void open_socket_locked();
  void close_socket_locked();

  std::string host_;
  uint16_t port_;

  std::mutex io_mutex_;
  std::intptr_t socket_fd_ = -1;
  bool subscribed_ = false;
  bool pool_handshake_done_ = false;
  std::optional<PublicKey> pool_handshake_public_;
};

} // namespace scrig
