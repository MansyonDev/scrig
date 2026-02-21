# SCRIG

High performance all 100% Open-Source SnapCoin Miner built in C++

---

# Features 

- `solo` mining against a Snap Node
- `pool` mining against Snap Coin Pools 
- Full RandomX support on every device and OS
- Clean live terminal dashboard
- Runtime commands: `h`, `p`, `r`, `q`
- Wallet balance and current block height tracking via active local Snap Node
- Os Configured, Specialized and Optimized `config.json`
- Easy to Use and Setup
- Works on every device with every OS
- 0% Miner Fee
- 100% Open Source built in C++

and many more...

---

# Setup and Running the Miner

## Windows (Manually)

1. Go to `Releases`
2. Download the latest `windows.zip` 
3. Unpack it and in the folder you will see `scrig.exe`

## Windows (CMD)

1. Open CMD
2. Download the latest `windows.zip` via this command

```bash
curl -L -o windows.zip https://github.com/MansyonDev/scrig/releases/latest/download/windows.zip
```

3. Unpack it 

```bash
tar -xf windows.zip
```

4. Enter the folder 

```bash
cd windows
```

When in the folder with `scrig.exe` just type this in cmd

```bash
.\scrig.exe
```

You will be prompted with an error saying

```bash
Created default config at: "config.json"
Set wallet_address in that file, then run scrig again.
```

Enter the `config.json` file and change:

- `"wallet_address":` to your snap-coin-wallet address 
- `"mode":` if you want to mine solo change to `solo` and set your node ip and port in `"node_host":` and `"node_port":` if you want to mine in an pool change to `pool` and set your pool ip and port in `"pool_host":` and `"pool_port":`

Rest of the config is optional (optimizations and ui), but worth adjusting to your specific CPU.
Here is an example config for Windows:

```json
{
  "_config_comment": "Windows Specific Config",
  "_profile": "windows-performance",
  "_note": "Set wallet_address before mining.",

  "wallet_address": "2ra6i1cmndm5p2hrjwt0gkb87ydd3uzjj522hpz95ghmct1wkv",
  "mode": "solo",

  "node_host": "127.0.0.1",
  "node_port": 3003,
  "pool_host": "pool.snap-coin.net",
  "pool_port": 3333,

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

I highly recommend when running `pool` mode to have an local node running, because you will be able to access features like wallet balance and current block height tracking. It is not essential but recommended.

## Linux (Manually)

1. Go to `Releases`
2. Download the latest `linux.zip` 
3. Unpack it and in the folder you will see `scrig`

## Linux (Terminal)

1. Open Terminal
2. Download the latest `linux.zip` via this command

```bash
curl -L -o linux.zip https://github.com/MansyonDev/scrig/releases/latest/download/linux.zip
```

3. Unpack it 

```bash
unzip linux.zip
```

4. Enter the folder 

```bash
cd linux
```

When in the folder with `scrig` just type this in the terminal

```bash
./scrig
```

You will be prompted with an error saying

```bash
Created default config at: "config.json"
Set wallet_address in that file, then run scrig again.
```

Enter the `config.json` file and change:

- `"wallet_address":` to your snap-coin-wallet address 
- `"mode":` if you want to mine solo change to `solo` and set your node ip and port in `"node_host":` and `"node_port":` if you want to mine in an pool change to `pool` and set your pool ip and port in `"pool_host":` and `"pool_port":` 

Rest of the config is optional (optimizations and ui), but worth adjusting to your specific CPU.
Here is an example config for Linux:

```json
{
  "_config_comment": "Linux Specific Config",
  "_profile": "linux-performance",
  "_note": "Set wallet_address before mining.",

  "wallet_address": "2ra6i1cmndm5p2hrjwt0gkb87ydd3uzjj522hpz95ghmct1wkv",
  "mode": "solo",

  "node_host": "127.0.0.1",
  "node_port": 3003,
  "pool_host": "pool.snap-coin.net",
  "pool_port": 3333,

  "threads": 0,
  "include_mempool_transactions": true,
  "refresh_interval_ms": 500,
  "use_chain_events": true,

  "pin_threads": true,
  "numa_bind": true,
  "randomx_full_mem": true,
  "randomx_huge_pages": true,
  "randomx_jit": true,
  "randomx_hard_aes": true,
  "randomx_secure": false,

  "colorful_ui": true,
  "dashboard": true
}
```

I highly recommend when running `pool` mode to have an local node running, because you will be able to access features like wallet balance and current block height tracking. It is not essential but recommended.

## Macos (Manually)

1. Go to `Releases`
2. Download the latest `macos.zip` 
3. Unpack it and in the folder you will see `scrig`

## Macos (Terminal)

1. Open Terminal
2. Download the latest `macos.zip` via this command

```bash
curl -L -o macos.zip https://github.com/MansyonDev/scrig/releases/latest/download/macos.zip
```

3. Unpack it 

```bash
unzip macos.zip
```

4. Enter the folder 

```bash
cd macos
```

When in the folder with `scrig` just type this in the terminal

```bash
./scrig
```

You will be prompted with an error saying

```bash
Created default config at: "config.json"
Set wallet_address in that file, then run scrig again.
```

Enter the `config.json` file and change:

- `"wallet_address":` to your snap-coin-wallet address 
- `"mode":` if you want to mine solo change to `solo` and set your node ip and port in `"node_host":` and `"node_port":` if you want to mine in an pool change to `pool` and set your pool ip and port in `"pool_host":` and `"pool_port":` 

Rest of the config is optional (optimizations and ui), but worth adjusting to your specific CPU.
Here is an example config for Macos:

```json
{
  "_config_comment": "macOS Specific Config",
  "_profile": "macos-stable",
  "_note": "Set wallet_address before mining.",

  "wallet_address": "2ra6i1cmndm5p2hrjwt0gkb87ydd3uzjj522hpz95ghmct1wkv",
  "mode": "solo",

  "node_host": "127.0.0.1",
  "node_port": 3003,
  "pool_host": "pool.snap-coin.net",
  "pool_port": 3333,

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

Important! ⚠️

- For the miner to work on Macos, the `"randomx_secure"`: must be set to `true`, and for best performance `"randomx_macos_unsafe":` should also be set to `true`

I highly recommend when running `pool` mode to have an local node running, because you will be able to access features like wallet balance and current block height tracking. It is not essential but recommended.

---

# Flags (optional)

Current Flags are:

1. `--config <path>`
Use a specific config file (default is config.json).

2. `--validate-node`
Validate node (and pool if mode=pool) then exit.

3. `--help`
Show help.

Syntax:

```bash
scrig [--config <path>] [--validate-node]
```

---

# Runtime Controls 

Inside Miner Terminal:

- `h` = print current hashrate 
- `p` = pause mining
- `r` = resume mining
- `q` = quit miner

---

# Dashboard/UI Notes

- `Mode` = tells the current mode you mine on `solo` for solo mining, `pool` for pool mining
- `Node` = tells the current node you are connected to. 
- `Optimization Profile` = tells the current optimizations enabled or disabled. 
- `Height` = tells the current block height on the blockchain
- `Threads` = tells the current active threads mining
- `Hashrate` = tells the current hashrate
- `Uptime` = tells how long the miner is mining for

In Pool Mode:

- `Shares Accpeted/Rejected` = tells how many shares have been accepted by the network and how much didn't

In Solo Mode:

- `Accepted/Rejected` = tells how many blocks have been accpeted by the network and how much didn't

- `Wallet (node)` = tells your current wallet balance (works only if local node is working and correct `node_host` and `node_port` is specified in `config.json`)
- `Last Block Hash` = tells the last block hash
- `Status` = tells the current status of the miner (`Starting` , `Mining` , `Mining Shares` , `Paused` , `Resuming` etc.)
- `Commands` = list of available commands for use
- `Runtime Log` = an log of what is happening etc.

--- 

# Donate

You can help keep motivation and further development by donating:

- `Snap Coin`: `2ra6i1cmndm5p2hrjwt0gkb87ydd3uzjj522hpz95qhmct1wkv`
- `Bitcoin`: `bc1ql2qvl40qwrlrr4f6lrtmlkut4gmnhz3svm6ssq`
- `Ethereum`: `0xe4937cEf33F76644e9099CC9f89cC2f019AA95e1`
- `Solana`: `FnFmo1826UdUhivzw871Tgu4MWUaLvn4fzy8eoJDRf89`

---

Snap Coin Website: `https://snap-coin.net`

---

# DISCLAIMER

SCRIG is provided for lawful, authorized use only. By using this software, you agree that:

1. You will use SCRIG only on devices, networks, and accounts you own or are explicitly permitted to use.
2. You are responsible for complying with all applicable laws, regulations, and pool/node terms in your country or region.
3. Mining performance, uptime, profitability, and rewards are not guaranteed.
4. Mining is resource-intensive and may cause high power usage, heat, system instability, or hardware wear; you assume all related risks.
5. You are solely responsible for securing your wallet address, keys, configuration, and system environment.
6. Antivirus/security tools may flag mining software; you are responsible for verifying binaries and source code integrity before running.
7. SCRIG is provided “AS IS”, without warranties of any kind, express or implied. The authors/contributors are not liable for any direct or indirect damages, losses, or claims arising from its use.

If you do not agree with these terms, do not use this software.