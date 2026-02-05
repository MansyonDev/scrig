#pragma once

#include "snap/requests.hpp"

namespace scrig::snap {

class Client {
public:
    Response fetch(const Request&);
};

}