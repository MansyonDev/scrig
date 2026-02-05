#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <array>
#include <nlohmann/json.hpp>

namespace scrig::snap {

struct BlockEvent {
    uint64_t height;
    std::string hash;
    nlohmann::json to_json() const;
};

struct TxEvent {
    std::string txid;
    nlohmann::json to_json() const;
};

}