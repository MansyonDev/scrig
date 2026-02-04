#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <optional>

namespace scrig::codec {

struct Bincode2Writer {
    std::vector<std::uint8_t> out;

    void u8(std::uint8_t v) { out.push_back(v); }

    void u32_le(std::uint32_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF);
        out.push_back((v >> 24) & 0xFF);
    }

    void u64_le(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back((v >> (8 * i)) & 0xFF);
    }

    void var_u64(std::uint64_t v) {
        while (v >= 0x80) {
            out.push_back(static_cast<std::uint8_t>(v) | 0x80);
            v >>= 7;
        }
        out.push_back(static_cast<std::uint8_t>(v));
    }

    void usize(std::size_t v) { var_u64(static_cast<std::uint64_t>(v)); }

    template<typename T>
    void vec(const std::vector<T>& v) {
        usize(v.size());
        for (const auto& e : v) encode(e);
    }

    void vec(const std::vector<std::uint8_t>& v) {
        usize(v.size());
        out.insert(out.end(), v.begin(), v.end());
    }

    template<typename T>
    void option(const std::optional<T>& v) {
        if (!v.has_value()) { u8(0); return; }
        u8(1);
        encode(*v);
    }

    template<typename T>
    void encode(const T& v) {
        v.bincode2_encode(*this);
    }
};

}