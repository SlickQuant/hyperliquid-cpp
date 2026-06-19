# hyperliquid-cpp

A C++20 SDK for the [Hyperliquid](https://hyperliquid.xyz) DEX API — REST market data, authenticated trading, and real-time WebSocket subscriptions, with native EIP-712 signing.

[![CI](https://github.com/SlickQuant/hyperliquid-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/SlickQuant/hyperliquid-cpp/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

---

## Overview

Hyperliquid is a high-performance on-chain perpetuals and spot DEX. This SDK provides typed C++ wrappers for the full Hyperliquid API surface, mirroring the structure of the [official Python SDK](https://github.com/hyperliquid-dex/hyperliquid-python-sdk):

- **`Info`** — read-only REST queries for market data and account state
- **`Exchange`** — authenticated order management, leverage, transfers, and sub-accounts
- **`WebsocketManager`** — real-time subscriptions (used internally by `Info::subscribe`)

All authenticated actions are signed locally using EIP-712 / secp256k1; your private key never leaves the process.

---

## Features

- Read-only market data (`Info`): universe metadata, mid prices, order books, candles, funding history, account state
- Authenticated trading (`Exchange`): limit/market/trigger orders, bulk ops, cancel, modify, leverage, margin, USD/spot transfers, bridge withdrawal
- Real-time WebSocket subscriptions via `Info::subscribe` with multi-callback fan-out per channel
- Native EIP-712 signing for both L1 actions (orders, leverage) and user-signed actions (transfers) using OpenSSL secp256k1
- `nlohmann::ordered_json` + msgpack action hashing — byte-for-byte compatible with the Python SDK signing output
- Embedded Keccak-256 (Ethereum's pre-FIPS variant — distinct from OpenSSL's SHA3-256)
- C++20; single umbrella include `<hyperliquid/hyperliquid.hpp>`; links as a static library

---

## Prerequisites

- C++20 compiler (MSVC 2022, GCC 13+, Clang 16+)
- CMake ≥ 3.20
- vcpkg with the required packages installed (see below)

### Installing vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg

# Linux / macOS
./bootstrap-vcpkg.sh

# Windows (PowerShell)
.\bootstrap-vcpkg.bat
```

### Installing required packages

```bash
vcpkg install nlohmann-json openssl gtest
```

`slick-net` is the HTTP/WebSocket library used by this SDK. Install it through its own distribution and register it in the same vcpkg instance:

```bash
vcpkg install slick-net
```

> **Note:** This project uses `find_package` via the CMake toolchain — no `vcpkg.json` is needed.
> Pass `-DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake` at configure time,
> or set it permanently in a `CMakePresets.json`.

---

## Building

```bash
# Configure (replace the toolchain path with your vcpkg location)
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build all targets
cmake --build build --config Debug
```

Targets produced:

| Target | Output |
|--------|--------|
| `hyperliquid` | Static library |
| `basic_order` | Example executable |
| `market_data_websocket` | Public market data WebSocket example |
| `market_data_websocket_per_coin` | Public market data WebSocket example using one connection per coin and slick-net logging hooks |
| `hyperliquid_tests` | Offline unit tests |
| `hyperliquid_integration_tests` | Live testnet integration tests |

### Linking in your own CMake project

```cmake
find_package(hyperliquid CONFIG REQUIRED)   # or add_subdirectory
target_link_libraries(my_app PRIVATE hyperliquid)
```

---

## Quick Start

### Include

```cpp
#include <hyperliquid/hyperliquid.hpp>   // umbrella header
```

### Read market data (no key required)

```cpp
auto info = std::make_shared<hyperliquid::Info>(hyperliquid::TESTNET_API_URL);

// All mid prices
auto mids = info->all_mids();
std::cout << "ETH mid: " << mids["ETH"] << "\n";

// L2 order book
auto book = info->l2_snapshot("BTC");
// book["levels"][0] = bids, book["levels"][1] = asks

// 1-hour candles for the last 24 hours
using namespace std::chrono;
int64_t now_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
auto candles = info->candle_snapshot("ETH", "1h", now_ms - 86400000LL);
```

### Authenticated trading

```cpp
auto info = std::make_shared<hyperliquid::Info>(
    hyperliquid::TESTNET_API_URL, /*skip_ws=*/true);

hyperliquid::Exchange exchange(
    "0xYOUR_PRIVATE_KEY_HEX",   // with or without "0x" prefix
    hyperliquid::TESTNET_API_URL,
    info);

std::cout << "Wallet: " << exchange.wallet_address() << "\n";

// Resting limit buy — 0.01 ETH at $1 100 (GTC)
auto result = exchange.order(
    "ETH", /*is_buy=*/true, /*sz=*/0.01, /*limit_px=*/1100.0,
    hyperliquid::LimitOrderType{hyperliquid::Tif::Gtc});

// Cancel if resting
if (result.value("status", "") == "ok") {
    const auto& statuses = result["response"]["data"]["statuses"];
    if (!statuses.empty() && statuses[0].contains("resting")) {
        int64_t oid = statuses[0]["resting"]["oid"].get<int64_t>();
        exchange.cancel("ETH", oid);
    }
}

// Market buy with 2% slippage
auto fill = exchange.market_open("ETH", /*is_buy=*/true, /*sz=*/0.01, /*slippage=*/0.02);

// Close entire ETH position at default 5% slippage
exchange.market_close("ETH");
```

### Real-time WebSocket

```cpp
// skip_ws defaults to false — WebSocket connects automatically
auto info = std::make_shared<hyperliquid::Info>(hyperliquid::TESTNET_API_URL);

// Subscribe to ETH order book updates
int sid = info->subscribe(
    {{"type", "l2Book"}, {"coin", "ETH"}},
    [](const nlohmann::json& msg) {
        // msg = {"channel": "l2Book", "data": {"coin": "ETH", "levels": [...]}}
        std::cout << msg["data"]["levels"][0].size() << " bids\n";
    });

// Multiple callbacks on the same channel are supported
int sid2 = info->subscribe(
    {{"type", "l2Book"}, {"coin", "ETH"}},
    [](const nlohmann::json& msg) { /* another handler */ });

// Unsubscribe by ID
info->unsubscribe({{"type", "l2Book"}, {"coin", "ETH"}}, sid);
info->unsubscribe({{"type", "l2Book"}, {"coin", "ETH"}}, sid2);
```

The WebSocket manager sends periodic pings to keep connections alive and uses
bounded atomic shutdown checks so teardown does not wait for the full ping interval.

---

## API Reference — Info

Construct with:

```cpp
hyperliquid::Info info(base_url, skip_ws = false);
```

Setting `skip_ws = true` disables the WebSocket connection (REST-only mode, lower overhead).

### Market data

| Method | Description |
|--------|-------------|
| `meta()` | Perpetuals universe metadata (names, size decimals, max leverage, …) |
| `spot_meta()` | Spot tokens and spot universe |
| `all_mids()` | All perpetuals mid prices as `{"BTC": "105000.0", "ETH": "2500.5", …}` |
| `l2_snapshot(coin)` | Full L2 order book snapshot; `levels[0]` = bids, `levels[1]` = asks |
| `candle_snapshot(coin, interval, start_ms)` | OHLCV candles from `start_ms` to now |
| `candle_snapshot(coin, interval, start_ms, end_ms)` | OHLCV candles in a time window |
| `perp_asset_ctxs()` | `[meta, assetCtxs]` — funding rate, OI, mark price for every perp |
| `spot_asset_ctxs()` | `[meta, assetCtxs]` for spot markets |
| `funding_history(coin, start_ms)` | Historical funding rates from `start_ms` |
| `funding_history(coin, start_ms, end_ms)` | Historical funding rates in a window |

Candle `interval` values: `"1m"` `"5m"` `"15m"` `"30m"` `"1h"` `"4h"` `"8h"` `"12h"` `"1d"` `"3d"` `"1w"`

### Account data

| Method | Description |
|--------|-------------|
| `user_state(address)` | Full account state: margin summary, cross/isolated positions |
| `open_orders(address)` | All open orders |
| `user_fills(address)` | Complete fill history |
| `user_fills_by_time(address, start_ms)` | Fills since `start_ms` |
| `user_fills_by_time(address, start_ms, end_ms)` | Fills in a window |
| `query_order_by_oid(address, oid)` | Single order status by order ID |
| `query_order_by_cloid(address, cloid)` | Single order status by client order ID |
| `sub_accounts(address)` | Sub-account list |

### WebSocket

| Method | Description |
|--------|-------------|
| `subscribe(sub_json, callback)` | Subscribe to a channel; returns `subscription_id` (positive int) |
| `unsubscribe(sub_json, subscription_id)` | Remove one callback; double-unsubscribe is a no-op |
| `load_meta()` | Populate `coin_to_asset`, `name_to_coin`, and `asset_to_sz_decimals` (called automatically by `Exchange`) |

`coin_to_asset` maps canonical wire coins to asset ids. Perps are 0-based; spot assets use `10000 + spot index`.
`name_to_coin` maps human-readable names such as `PURR/USDC` back to canonical wire coins.

---

## API Reference — Exchange

Construct with:

```cpp
hyperliquid::Exchange exchange(
    private_key_hex,    // "0x..." or bare 64 hex chars
    base_url,           // MAINNET_API_URL or TESTNET_API_URL
    info,               // shared_ptr<Info>
    vault_address,      // optional sub-account address
    account_address);   // optional account queried when using an API wallet
```

The constructor derives `wallet_address()` from the private key and calls `info->load_meta()`.

All methods return `nlohmann::json` with the raw Hyperliquid API response.

### Orders

| Method | Description |
|--------|-------------|
| `order(coin, is_buy, sz, limit_px, order_type, reduce_only, cloid, builder, grouping)` | Place a single order |
| `bulk_orders(orders, builder, grouping)` | Place multiple orders atomically |
| `market_open(coin, is_buy, sz, slippage=0.05, cloid)` | IoC limit at mid ± slippage |
| `market_close(coin, sz={}, slippage=0.05, cloid)` | Close position (size from `user_state` if omitted) |

### Cancel

| Method | Description |
|--------|-------------|
| `cancel(coin, oid)` | Cancel a single order by order ID |
| `bulk_cancel(cancels)` | Cancel multiple orders |
| `cancel_by_cloid(coin, cloid)` | Cancel by client order ID |
| `bulk_cancel_by_cloid(cancels)` | Cancel multiple orders by client order ID |
| `schedule_cancel(time_ms)` | Dead-man's switch — cancels all orders at `time_ms` (omit for immediate) |

### Modify

| Method | Description |
|--------|-------------|
| `modify_order(oid, new_order)` | Replace a resting order |
| `bulk_modify_orders(mods)` | Replace multiple resting orders |

### Leverage & margin

| Method | Description |
|--------|-------------|
| `update_leverage(coin, is_cross, leverage)` | Set cross or isolated leverage |
| `update_isolated_margin(coin, amount)` | Add/remove USD from an isolated position |

### Transfers (EIP-712 user-signed)

| Method | Description |
|--------|-------------|
| `usd_class_transfer(amount, to_perp)` | Move USDC between spot and perpetuals vault |
| `usd_transfer(amount, destination)` | Send USDC to another address |
| `spot_transfer(amount, destination, token)` | Send spot tokens to another address |
| `withdraw_from_bridge(amount, destination)` | Initiate L1 bridge withdrawal |

### Agent & builder

| Method | Description |
|--------|-------------|
| `approve_agent(agent_address, agent_name="")` | Authorize an agent key |
| `approve_builder_fee(builder, max_fee_rate)` | Approve builder fee (rate as `"0.001%"` string) |

### Sub-accounts

| Method | Description |
|--------|-------------|
| `create_sub_account(name)` | Create a named sub-account |
| `sub_account_transfer(usd, to_sub, sub_account_user)` | Transfer USDC between main and sub |

---

## Types Reference

### Endpoints

```cpp
hyperliquid::MAINNET_API_URL   // "https://api.hyperliquid.xyz"
hyperliquid::TESTNET_API_URL   // "https://api.hyperliquid-testnet.xyz"
hyperliquid::LOCAL_API_URL     // "http://localhost:3001"
```

### Time-in-Force

```cpp
hyperliquid::Tif::Gtc   // Good till cancel
hyperliquid::Tif::Ioc   // Immediate or cancel
hyperliquid::Tif::Alo   // Add liquidity only (post-only)
```

### Order types

```cpp
// Standard limit order
hyperliquid::LimitOrderType{hyperliquid::Tif::Gtc}

// Stop / TP-SL trigger
hyperliquid::TriggerOrderType{
    .trigger_px = 2000.0,
    .is_market  = true,     // true = market fill on trigger, false = limit
    .tpsl       = "sl",     // "tp" or "sl"
}
```

`OrderType` is `std::variant<LimitOrderType, TriggerOrderType>`.

### Client order ID (Cloid)

```cpp
auto cloid = hyperliquid::Cloid::from_int(42);
auto cloid = hyperliquid::Cloid::from_str("0x0000000000000000000000000000002a");
```

### Builder fee

```cpp
hyperliquid::BuilderInfo{
    .b = "0xBUILDER_ADDRESS",
    .f = 10,   // tenths of basis points (10 = 1 bps = 0.01%)
}
```

---

## WebSocket Channels

Subscription objects follow the Python SDK's JSON format:

| Subscription | Channel identifier | Description |
|---|---|---|
| `{"type":"allMids"}` | `allMids` | All mid prices — fires on every top-of-book change |
| `{"type":"l2Book","coin":"ETH"}` | `l2Book:ETH` | Full order book updates |
| `{"type":"bbo","coin":"ETH"}` | `bbo:ETH` | Best bid/offer only |
| `{"type":"trades","coin":"ETH"}` | `trades:ETH` | Public trade feed |
| `{"type":"candle","coin":"BTC","interval":"1m"}` | `candle:BTC,1m` | Candle updates |
| `{"type":"activeAssetCtx","coin":"ETH"}` | `activeAssetCtx:ETH` | Funding / mark price updates |
| `{"type":"userEvents","user":"0x..."}` | `userEvents:0x...` | Fills, liquidations, funding |
| `{"type":"userFills","user":"0x..."}` | `userFills:0x...` | Fill stream |
| `{"type":"orderUpdates","user":"0x..."}` | `orderUpdates:0x...` | Order status changes |
| `{"type":"userFundings","user":"0x..."}` | `userFundings:0x...` | Funding payments |
| `{"type":"userNonFundingLedgerUpdates","user":"0x..."}` | `userNonFundingLedgerUpdates:0x...` | Non-funding ledger events |
| `{"type":"webData2","user":"0x..."}` | `webData2:0x...` | Aggregated UI data |
| `{"type":"activeAssetData","coin":"ETH","user":"0x..."}` | `activeAssetData:ETH,0x...` | Per-asset user context |

> **Note:** BBO is a WebSocket-only channel. The REST `Info::bbo()` method is not supported by the testnet endpoint.

Each callback receives the full message object:

```json
{
  "channel": "l2Book",
  "data": {
    "coin": "ETH",
    "levels": [ ["...bids..."], ["...asks..."] ]
  }
}
```

---

## Running Examples

```bash
# Using the hardhat test key (no real funds)
./build/Debug/basic_order

# Using your own key
./build/Debug/basic_order 0xYOUR_PRIVATE_KEY_HEX

# Subscribe to allMids and l2Book updates on one WebSocket connection
./build/Debug/market_data_websocket ETH BTC --seconds 30

# Subscribe to ETH and BTC l2Book updates using one WebSocket connection per coin
./build/Debug/market_data_websocket_per_coin 30
```

`basic_order` places a resting limit buy on testnet and cancels it if it is resting.
The market data examples subscribe to public testnet WebSocket feeds and print
received updates for the requested duration.
`market_data_websocket_per_coin` also demonstrates configuring slick-net's
runtime logging hooks and routing `LOG_INFO` / `LOG_ERROR` output to `std::cout`.

---

## Testing

### Unit tests (offline, no network)

```bash
cmake --build build --config Debug --target hyperliquid_tests
ctest --test-dir build -C Debug -R hyperliquid_tests -V
```

Covers: Keccak-256 vectors, EIP-712 signing round-trips, type serialisation (`float_to_wire`, `Cloid`, `Tif`), WebSocket URL conversion, and channel identifier generation.

### Integration tests (requires testnet)

```bash
cmake --build build --config Debug --target hyperliquid_integration_tests
ctest --test-dir build -C Debug -R hyperliquid_integration_tests -V
```

Covers:
- **REST** (`InfoIntegration`, ~36 tests): `meta`, `spot_meta`, `all_mids`, `l2_snapshot`, `candle_snapshot`, `perp_asset_ctxs`, `spot_asset_ctxs`, `funding_history`, `user_state`, `open_orders`, `user_fills`, `sub_accounts`, `coin_to_asset` consistency
- **WebSocket** (`WsIntegration`, ~18 tests): `allMids` and `l2Book` message delivery, channel field validation, subscription ID uniqueness, multi-callback fan-out, simultaneous channels, unsubscribe delivery stop, subscribe/unsubscribe lifecycle

---

## License

[MIT](LICENSE) — Copyright (c) 2026 Slick Quant
