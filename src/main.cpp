#include <iostream>
#include <thread>
#include <array>
#include <cstdint>

#include "util/time.hpp"
#include "rx/randomx_wrap.hpp"

int main() {
    std::cout << "scrig starting...\n";

    uint64_t t1 = scrig::util::unix_time_millis();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t t2 = scrig::util::unix_time_millis();

    std::cout << "time diff ms: " << (t2 - t1) << "\n";

    scrig::rx::RandomX::init(false);

    const std::array<std::uint8_t, 4> msg{ 't','e','s','t' };
    auto h = scrig::rx::RandomX::hash(msg);
    std::cout << "hash[0]: " << int(h[0]) << "\n";

    return 0;
}