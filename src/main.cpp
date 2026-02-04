#include <randomx.h>
#include <iostream>

int main() {
    randomx_flags flags = randomx_get_flags();
    randomx_cache* cache = randomx_alloc_cache(flags);

    const char* seed = "test seed";
    randomx_init_cache(cache, seed, strlen(seed));

    randomx_vm* vm = randomx_create_vm(flags, cache, nullptr);

    uint8_t hash[32];
    const char* input = "hello";
    randomx_calculate_hash(vm, input, strlen(input), hash);

    std::cout << "RandomX OK. Hash[0] = " << (int)hash[0] << std::endl;

    randomx_destroy_vm(vm);
    randomx_release_cache(cache);
}