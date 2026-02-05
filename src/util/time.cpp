#include "util/time.hpp"
#include <chrono>

namespace scrig::util {

uint64_t unix_time_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

}