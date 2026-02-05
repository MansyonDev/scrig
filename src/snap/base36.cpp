#include "base36.hpp"
#include <algorithm>

namespace scrig::snap::base36 {

static int v36(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    return -1;
}

static void mul_add_256(std::array<uint8_t, 32>& x, uint32_t mul, uint32_t add) {
    uint32_t carry = add;
    for (int i = 31; i >= 0; --i) {
        uint32_t v = static_cast<uint32_t>(x[i]) * mul + carry;
        x[i] = static_cast<uint8_t>(v & 0xFFu);
        carry = v >> 8;
    }
}

static uint32_t divmod_256(std::array<uint8_t, 32>& x, uint32_t div) {
    uint32_t rem = 0;
    for (int i = 0; i < 32; ++i) {
        uint32_t cur = (rem << 8) | x[i];
        x[i] = static_cast<uint8_t>(cur / div);
        rem = cur % div;
    }
    return rem;
}

std::optional<std::array<uint8_t, 32>> decode_32(const std::string& s) {
    std::array<uint8_t, 32> out{};
    bool any = false;

    for (char c : s) {
        int d = v36(c);
        if (d < 0) return std::nullopt;
        any = true;
        mul_add_256(out, 36u, static_cast<uint32_t>(d));
    }

    if (!any) return std::nullopt;
    return out;
}

std::string encode_32(const std::array<uint8_t, 32>& bytes) {
    std::array<uint8_t, 32> tmp = bytes;

    bool all_zero = true;
    for (auto b : tmp) if (b != 0) { all_zero = false; break; }
    if (all_zero) return "0";

    std::string out;
    while (true) {
        bool z = true;
        for (auto b : tmp) if (b != 0) { z = false; break; }
        if (z) break;

        uint32_t rem = divmod_256(tmp, 36u);
        char c = rem < 10 ? static_cast<char>('0' + rem) : static_cast<char>('a' + (rem - 10));
        out.push_back(c);
    }

    std::reverse(out.begin(), out.end());
    return out;
}

}