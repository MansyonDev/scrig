#include "snap/requests.hpp"

namespace scrig::snap {

nlohmann::json request_to_json(const Request&) {
    return nlohmann::json::object();
}

Request request_from_json(const nlohmann::json&) {
    return ReqHeight{};
}

nlohmann::json response_to_json(const Response&) {
    return nlohmann::json::object();
}

Response response_from_json(const nlohmann::json&) {
    return RespHeight{};
}

}