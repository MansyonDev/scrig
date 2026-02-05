#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <array>
#include <variant>

#include <nlohmann/json.hpp>

namespace scrig::snap {

// forward declarations (real definitions later)
struct Block {};
struct Transaction {};

struct ReqHeight {};
struct ReqDifficulty {};
struct ReqPeers {};
struct ReqReward {};
struct ReqMempool { std::uint32_t page{}; };
struct ReqNewBlock { Block new_block; };
struct ReqSubscribeToChainEvents {};

using Request = std::variant<
    ReqHeight,
    ReqDifficulty,
    ReqPeers,
    ReqReward,
    ReqMempool,
    ReqNewBlock,
    ReqSubscribeToChainEvents
>;

struct RespHeight { std::uint64_t height{}; };
struct RespDifficulty {
    std::array<std::uint8_t, 32> transaction_difficulty{};
    std::array<std::uint8_t, 32> block_difficulty{};
};
struct RespPeers { std::vector<std::string> peers; };
struct RespReward { std::uint64_t reward{}; };
struct RespMempool {
    std::vector<Transaction> mempool;
    std::optional<std::uint32_t> next_page;
};
struct RespNewBlock { nlohmann::json status; };
struct RespChainEvent { nlohmann::json event; };

using Response = std::variant<
    RespHeight,
    RespDifficulty,
    RespPeers,
    RespReward,
    RespMempool,
    RespNewBlock,
    RespChainEvent
>;

nlohmann::json request_to_json(const Request&);
Request request_from_json(const nlohmann::json&);

nlohmann::json response_to_json(const Response&);
Response response_from_json(const nlohmann::json&);

}