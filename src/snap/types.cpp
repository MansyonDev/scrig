#include "snap/types.hpp"

using nlohmann::json;

namespace scrig::snap {

json BlockEvent::to_json() const {
    json j = json::object();
    j["type"] = "block";
    j["height"] = height;
    j["hash"] = hash;
    return j;
}

json TxEvent::to_json() const {
    json j = json::object();
    j["type"] = "transaction";
    j["txid"] = txid;
    return j;
}

}