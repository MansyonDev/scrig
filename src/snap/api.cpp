#include "snap/api.hpp"

namespace scrig::snap {

Response Client::fetch(const Request&) {
    return RespHeight{};
}

}