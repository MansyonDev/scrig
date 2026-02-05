#include "util.hpp"
#include "../rx/randomx_wrap.hpp"
#include <algorithm>

namespace scrig::snap {

static constexpr std::size_t MAX_TRANSACTION_IO = 150;
static constexpr std::size_t MAX_TRANSACTIONS_PER_BLOCK = 500;

Transaction build_transaction(
  Client& provider,
  const PrivateKey& sender,
  std::vector<std::pair<PublicKey, std::uint64_t>> receivers,
  const std::vector<TransactionInput>& ignore_inputs
) {
  std::uint64_t target_balance = 0;
  for(auto& r : receivers) target_balance += r.second;

  auto utxos = provider.get_available_utxos(sender.to_public());

  utxos.erase(
    std::remove_if(utxos.begin(), utxos.end(),
      [&](auto& u){
        for(auto& ig : ignore_inputs) {
          if(ig.transaction_id.b == u.transaction_id.b &&
             ig.output_index == u.output_index) return true;
        }
        return false;
      }),
    utxos.end()
  );

  std::vector<decltype(utxos[0])> used;
  std::uint64_t funds = 0;

  for(auto& u : utxos) {
    used.push_back(u);
    funds += u.output.amount;
    if(funds >= target_balance) break;
  }

  if(funds < target_balance)
    throw UtilError("insufficient funds");

  if(funds > target_balance)
    receivers.emplace_back(sender.to_public(), funds - target_balance);

  if(used.size() + receivers.size() > MAX_TRANSACTION_IO)
    throw UtilError("too many inputs/outputs");

  std::sort(used.begin(), used.end(),
    [](auto& a, auto& b){
      return a.output.amount > b.output.amount;
    });

  Transaction tx;
  tx.timestamp = unix_time_seconds();
  tx.nonce = 0;

  for(auto& u : used) {
    TransactionInput in;
    in.transaction_id = u.transaction_id;
    in.output_index = u.output_index;
    in.output_owner = sender.to_public();
    tx.inputs.push_back(in);
  }

  for(auto& r : receivers) {
    TransactionOutput o;
    o.amount = r.second;
    o.receiver = r.first;
    tx.outputs.push_back(o);
  }

  tx.sign(sender);
  return tx;
}

Block build_block(
  Client& provider,
  const std::vector<Transaction>& txs,
  const PublicKey& miner
) {
  auto height = provider.get_height();
  auto reward = get_block_reward(height);

  std::vector<Transaction> transactions = txs;

  Transaction reward_tx;
  reward_tx.timestamp = unix_time_seconds();
  reward_tx.nonce = 0;

  TransactionOutput dev;
  dev.amount = calculate_dev_fee(reward);
  dev.receiver = DEV_WALLET;

  TransactionOutput miner_out;
  miner_out.amount = reward - dev.amount;
  miner_out.receiver = miner;

  reward_tx.outputs.push_back(dev);
  reward_tx.outputs.push_back(miner_out);

  auto tx_diff = provider.get_transaction_difficulty();
  reward_tx.compute_pow(tx_diff);

  transactions.push_back(reward_tx);

  if(transactions.size() > MAX_TRANSACTIONS_PER_BLOCK)
    throw UtilError("too many transactions");

  std::vector<Hash32> ids;
  for(auto& tx : transactions) {
    if(!tx.transaction_id.has_value())
      throw UtilError("transaction incomplete");
    ids.push_back(*tx.transaction_id);
  }

  auto merkle = MerkleTree::build(ids);
  auto filter = AddressInclusionFilter::create(transactions);

  auto prev = provider.get_block_hash_by_height(
    height == 0 ? 0 : height - 1
  );

  Block b;
  b.transactions = transactions;
  b.timestamp = unix_time_seconds();
  b.nonce = 0;

  b.meta.block_pow_difficulty = provider.get_block_difficulty();
  b.meta.tx_pow_difficulty = provider.get_transaction_difficulty();
  b.meta.previous_block = prev.value_or(GENESIS_PREVIOUS_BLOCK_HASH);
  b.meta.merkle_tree_root = merkle.root;
  b.meta.address_inclusion_filter = filter;

  return b;
}

}