# scrig

High-performance Snap Coin miner in C++ with:
- `solo` mining against a Snap node
- `pool` mining against `snap-coin-pool`
- RandomX support
- OS-aware auto-tuning and OS-specific `config.json` generation
- Live terminal dashboard + hotkeys

## Quick Start

1. Get a binary from [Releases](https://github.com/MansyonDev/scrig/releases) (recommended), or build from source.
2. Run `scrig` once to auto-generate `config.json`.
3. Edit `wallet_address`, `mode`, and endpoints.
4. Validate connectivity.
5. Start mining.

---

## Features

- Snap API framing and request/response compatibility
- Pool handshake + chain event job stream
- Multi-threaded mining with CPU affinity controls
- Runtime optimization sanitization (unsupported flags are auto-disabled)
- OS-specific config profile header:
  - `"_config_comment": "macOS Specific Config"`
  - `"_config_comment": "Windows Specific Config"`
  - `"_config_comment": "Linux Specific Config"`
- `--validate-node` mode
- Runtime commands: `h`, `p`, `r`, `q`

---

## Install From Releases

### macOS

#### GUI
1. Open [Releases](https://github.com/MansyonDev/scrig/releases).
2. Download the macOS archive.
3. Extract it.
4. Open Terminal in that folder.
5. Run:

```bash
chmod +x scrig
./scrig
```

#### CLI only

```bash
# Option A: GitHub CLI
brew install gh

gh release download --repo MansyonDev/scrig --pattern "*macos*" --dir .
# Extract downloaded archive, then:
chmod +x scrig
./scrig
```

### Windows

#### GUI
1. Open [Releases](https://github.com/MansyonDev/scrig/releases).
2. Download the Windows archive.
3. Extract it.
4. Open `cmd` or PowerShell in the extracted folder.
5. Run:

```powershell
.\scrig.exe
```

#### CLI only (PowerShell)

```powershell
winget install --id GitHub.cli -e

gh release download --repo MansyonDev/scrig --pattern "*windows*" --dir .
# Extract archive, then:
.\scrig.exe
```

### Linux

#### GUI
1. Open [Releases](https://github.com/MansyonDev/scrig/releases).
2. Download the Linux archive.
3. Extract it.
4. Open Terminal in that folder.
5. Run:

```bash
chmod +x scrig
./scrig
```

#### CLI only

```bash
# Option A: GitHub CLI
sudo apt-get update
sudo apt-get install -y gh

gh release download --repo MansyonDev/scrig --pattern "*linux*" --dir .
# Extract archive, then:
chmod +x scrig
./scrig
```

---

## Build From Source (CLI)

### Requirements

- CMake 3.20+
- C++20 compiler
- Git

Optional for best performance:
- RandomX installed, or use vendored `third_party/RandomX`
- Linux NUMA (`libnuma-dev`)

### macOS

```bash
xcode-select --install
brew install cmake

git clone git@github.com:MansyonDev/scrig.git
cd scrig
cmake -S . -B build
cmake --build build -j
./build/scrig
```

### Linux (Debian/Ubuntu)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config

# Optional NUMA
sudo apt-get install -y libnuma-dev

git clone git@github.com:MansyonDev/scrig.git
cd scrig
cmake -S . -B build
cmake --build build -j
./build/scrig
```

### Windows (Developer PowerShell)

```powershell
git clone git@github.com:MansyonDev/scrig.git
cd scrig
cmake -S . -B build
cmake --build build --config Release
.\build\Release\scrig.exe
```

### Build with explicit RandomX root

```bash
cmake -S . -B build -DRANDOMX_ROOT=/path/to/randomx/install
cmake --build build -j
```

---

## First Run + Config

On first launch, scrig creates `config.json` in the current working directory.

Rules for every OS:
- Config is generated for the current OS profile.
- Unsupported optimization keys are omitted from the file.
- Unsupported runtime flags are auto-disabled and saved back automatically.
- Wallet must be raw base36 address text only (no wrappers).

Correct wallet format:
```json
"wallet_address": "2ra6i1cmndm5p2hrjwt0gkb87ydd3uzjj522hpz95ghmct1wkv"
```

Wrong wallet format:
```json
"wallet_address": "<wallet>"
"wallet_address": "(wallet)"
```

### macOS

First run (release binary):
```bash
./scrig
```

First run (source build):
```bash
./build/scrig
```

Edit config:
```bash
open -a TextEdit config.json
```

Typical macOS-generated config:
```json
{
  "_config_comment": "macOS Specific Config",
  "_profile": "macos-stable",
  "_note": "Set wallet_address before mining.",

  "wallet_address": "<PUT_YOUR_WALLET_ADDRESS>",
  "mode": "solo",

  "node_host": "127.0.0.1",
  "node_port": 3003,
  "pool_host": "127.0.0.1",
  "pool_port": 3003,

  "threads": 0,
  "include_mempool_transactions": false,
  "refresh_interval_ms": 500,
  "use_chain_events": true,

  "pin_threads": true,
  "randomx_full_mem": true,
  "randomx_jit": true,
  "randomx_hard_aes": true,
  "randomx_secure": true,
  "randomx_macos_unsafe": true,

  "colorful_ui": true,
  "dashboard": true
}
```

### Windows

First run (release binary):
```powershell
.\scrig.exe
```

First run (source build):
```powershell
.\build\Release\scrig.exe
```

Edit config:
```powershell
notepad .\config.json
```

Typical Windows-generated config:
```json
{
  "_config_comment": "Windows Specific Config",
  "_profile": "windows-performance",
  "_note": "Set wallet_address before mining.",

  "wallet_address": "<PUT_YOUR_WALLET_ADDRESS>",
  "mode": "solo",

  "node_host": "127.0.0.1",
  "node_port": 3003,
  "pool_host": "127.0.0.1",
  "pool_port": 3003,

  "threads": 0,
  "include_mempool_transactions": true,
  "refresh_interval_ms": 500,
  "use_chain_events": true,

  "pin_threads": true,
  "randomx_full_mem": true,
  "randomx_huge_pages": false,
  "randomx_jit": true,
  "randomx_hard_aes": true,
  "randomx_secure": false,

  "colorful_ui": true,
  "dashboard": true
}
```

### Linux

First run (release binary):
```bash
./scrig
```

First run (source build):
```bash
./build/scrig
```

Edit config:
```bash
nano config.json
```

Typical Linux-generated config:
```json
{
  "_config_comment": "Linux Specific Config",
  "_profile": "linux-performance",
  "_note": "Set wallet_address before mining.",

  "wallet_address": "<PUT_YOUR_WALLET_ADDRESS>",
  "mode": "solo",

  "node_host": "127.0.0.1",
  "node_port": 3003,
  "pool_host": "127.0.0.1",
  "pool_port": 3003,

  "threads": 0,
  "include_mempool_transactions": true,
  "refresh_interval_ms": 500,
  "use_chain_events": true,

  "pin_threads": true,
  "numa_bind": false,
  "randomx_full_mem": true,
  "randomx_huge_pages": false,
  "randomx_jit": true,
  "randomx_hard_aes": true,
  "randomx_secure": false,

  "colorful_ui": true,
  "dashboard": true
}
```

---

## Run Modes

### Solo mode

```json
"mode": "solo"
```

Uses `node_host:node_port` for jobs + block submission.

### Pool mode

```json
"mode": "pool"
```

Uses:
- `pool_host:pool_port` for share/job protocol
- `node_host:node_port` for chain height and wallet balance display

---

## Validate Connectivity

```bash
./build/scrig --config config.json --validate-node
```

In pool mode, this also validates the pool handshake and pool difficulty exchange.

---

## Runtime Controls

Inside miner terminal:
- `h` = print hashrate snapshot
- `p` = pause mining
- `r` = resume mining
- `q` = clean quit

---

## Dashboard Notes

- `Wallet (node)` is balance reported by your configured `node_host:node_port`.
- If wallet app shows something different, check:
  - same address
  - same network
  - same node/chain

Pool mode:
- `Shares Accepted` means accepted pool shares.
- Shares are not immediate payouts.
- Payout depends on pool finding blocks and payout policy.

---

## Troubleshooting

### `missing CMakeCache.txt`

Run configure first:

```bash
cmake -S . -B build
cmake --build build -j
```

### `randomx_alloc_cache failed` / RandomX startup failure

- Check RandomX availability.
- Try lower-risk flags in config.
- Rebuild with explicit `-DRANDOMX_ROOT=...`.

### Height is `0` in pool mode

- Check `node_host/node_port` reachability.
- Pool mining can still work, but dashboard height needs node query access.

### Terminal rendering issues

- Use a modern terminal (Terminal.app, iTerm2, Windows Terminal).
- Avoid very small terminal window sizes.

---

## Performance Tips

- Keep `randomx_full_mem`, `randomx_jit`, `randomx_hard_aes` enabled when stable.
- Use `threads: 0` to auto-match logical CPU count.
- Enable huge pages where supported and configured.
- Keep pool/event connectivity stable to avoid stale-job overhead.

---

## Donate

Snap Coin : `2ra6i1cmndm5p2hrjwt0gkb87ydd3uzjj522hpz95qhmct1wkv`

Bitcoin : `bc1ql2qvl40qwrlrr4f6lrtmlkut4gmnhz3svm6ssq`

Ethereum : `0xe4937cEf33F76644e9099CC9f89cC2f019AA95e1`

Solana : `FnFmo1826UdUhivzw871Tgu4MWUaLvn4fzy8eoJDRf89`