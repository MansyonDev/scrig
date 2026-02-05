#include "randomx_wrap.hpp"
#include <mutex>
#include <atomic>
#include <vector>
#include <cstring>

extern "C" {
#include "randomx.h"
}

namespace scrig::rx {

static constexpr const char* SEED = "snap-coin-testnet";

static std::once_flag g_once;
static std::atomic<bool> g_full{false};
static randomx_dataset* g_dataset = nullptr;

static randomx_cache* make_cache(randomx_flags flags) {
    randomx_cache* c = randomx_alloc_cache(flags);
    if (!c) std::terminate();
    randomx_init_cache(c, SEED, std::strlen(SEED));
    return c;
}

static void init_full_dataset() {
    randomx_flags flags = randomx_get_flags();
    flags = (randomx_flags)(flags | RANDOMX_FLAG_JIT | RANDOMX_FLAG_FULL_MEM);

    randomx_cache* cache = make_cache(flags);
    g_dataset = randomx_alloc_dataset(flags);
    if (!g_dataset) std::terminate();

    unsigned long count = randomx_dataset_item_count();
    unsigned long threads = 1;
    unsigned long per = count / threads;

    for (unsigned long t = 0; t < threads; ++t) {
        unsigned long start = t * per;
        unsigned long items = (t == threads - 1) ? (count - start) : per;
        randomx_init_dataset(g_dataset, cache, start, items);
    }

    randomx_release_cache(cache);
}

void RandomX::init(bool full_mode) {
    g_full.store(full_mode, std::memory_order_release);
    if (full_mode) {
        std::call_once(g_once, [] { init_full_dataset(); });
    }
}

static thread_local randomx_vm* tl_vm = nullptr;
static thread_local randomx_cache* tl_cache = nullptr;

static randomx_vm* get_vm() {
    randomx_flags flags = randomx_get_flags();
    flags = (randomx_flags)(flags | RANDOMX_FLAG_JIT);

    if (g_full.load(std::memory_order_acquire)) {
        flags = (randomx_flags)(flags | RANDOMX_FLAG_FULL_MEM);
        if (!tl_vm) {
            if (!g_dataset) std::terminate();
            tl_vm = randomx_create_vm(flags, nullptr, g_dataset);
            if (!tl_vm) std::terminate();
        }
        return tl_vm;
    } else {
        if (!tl_cache) tl_cache = make_cache(flags);
        if (!tl_vm) {
            tl_vm = randomx_create_vm(flags, tl_cache, nullptr);
            if (!tl_vm) std::terminate();
        }
        return tl_vm;
    }
}

std::array<uint8_t, 32> RandomX::hash(std::span<const uint8_t> data) {
    randomx_vm* vm = get_vm();
    std::array<uint8_t, 32> out{};
    randomx_calculate_hash(vm, data.data(), data.size(), out.data());
    return out;
}

}