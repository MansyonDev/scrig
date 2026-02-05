#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>

#include "types.hpp"
#include "api.hpp"

namespace scrig::snap {

struct UtilError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

Transaction build_transaction(
  Client& provider,
  const PrivateKey& sender,
  std::vector<std::pair<PublicKey, std::uint64_t>> receivers,
  const std::vector<TransactionInput>& ignore_inputs
);

Block build_block(
  Client& provider,
  const std::vector<Transaction>& transactions,
  const PublicKey& miner
);

}