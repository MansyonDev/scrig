# scrig

`scrig` is a C++ Snap Coin miner with solo and pool mode support.

## Implemented

- Snap Coin API framing: `4-byte big-endian length + JSON payload`
- Current request/response variant format (`Height`, `Difficulty`, `Mempool`, `NewBlock`, `SubscribeToChainEvents`, ...)
- Base36 compatibility for wallet/public keys, hashes, signatures
- `config.json` auto-created on first run
- Solo mining flow:
  - fetch chain state from node
  - build candidate block locally
  - mine reward transaction PoW
  - mine block nonce and submit
- Pool mining flow:
  - pool handshake (`send miner public key bytes`, `read 32-byte pool difficulty`)
  - subscribe to pool chain events for jobs
  - mine shares/blocks and submit via `NewBlock`
- Performance controls:
  - multi-threaded mining
  - optional thread pinning
  - event-driven stale-job detection for solo mode
  - RandomX flags: full mem, huge pages, JIT, hard AES, secure mode
- Live terminal dashboard with hashrate and acceptance stats
- `--validate-node` connectivity check mode

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

If `cmake --build build` fails with `missing CMakeCache.txt`, the build directory was not configured yet. Re-run configure first:

```bash
cmake -S . -B build -DRANDOMX_ROOT=/path/to/randomx/install
cmake --build build -j4
```

Run:

```bash
./build/scrig
```

Runtime control:
- `Ctrl+Q` or `q` requests a clean miner shutdown.

## Config

Generated automatically as `config.json` if missing.

```json
{
  "wallet_address": "<YOUR_PUBLIC_WALLET_ADDRESS_BASE36>",
  "node_host": "127.0.0.1",
  "node_port": 3003,
  "mode": "solo",
  "pool_host": "127.0.0.1",
  "pool_port": 3003,
  "threads": 12,
  "include_mempool_transactions": true,
  "refresh_interval_ms": 500,
  "use_chain_events": true,
  "pin_threads": true,
  "numa_bind": false,
  "randomx_full_mem": true,
  "randomx_huge_pages": true,
  "randomx_jit": true,
  "randomx_hard_aes": true,
  "randomx_secure": false,
  "randomx_macos_unsafe": false,
  "colorful_ui": true,
  "dashboard": true
}
```

- `mode = "solo"`: mines directly against node.
- `mode = "pool"`: mines pool jobs from `pool_host:pool_port`.

## Validate Endpoint

```bash
./build/scrig --validate-node
```

This validates API connectivity and prints current chain data. In pool mode it also validates handshake and pool difficulty exchange.

## RandomX Wiring

Real mining requires RandomX.

Detection order:
1. `third_party/RandomX` (vendored by default in this repo)
2. `RANDOMX_ROOT` CMake prefix
3. standard include/lib locations (`/opt/homebrew`, `/usr/local`, `/usr`)

Build with explicit prefix:

```bash
cmake -S . -B build -DRANDOMX_ROOT=/path/to/randomx/install
cmake --build build -j
```

If RandomX is not found, scrig builds with fallback hashing for local testing only (not chain-compatible).

## Notes

- Best hashrate requires RandomX full memory mode and huge pages configured in OS.
- If full-memory or large-pages RandomX init fails, scrig automatically falls back to safer flags (including light mode as last resort) instead of crashing.
- scrig auto-disables unsupported tuning flags at startup (for example, huge pages on macOS or NUMA binding when unavailable) instead of forcing incompatible settings.
- On macOS, `randomx_jit` and `randomx_full_mem` are auto-disabled by default to avoid `SIGBUS` stability issues seen on some machines; set `randomx_macos_unsafe=true` to force those options.
- `use_chain_events=true` is the lowest-latency way to refresh solo jobs on new blocks.
- `numa_bind=true` can improve locality on Linux machines with multiple NUMA nodes (requires libnuma).
- Pool compatibility is implemented against `snap-coin-pool` protocol in the upstream repo.
