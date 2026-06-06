# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.1.0] — 2026-06-06

### Added

#### Core library (`hyperliquid`)

**`Info` — read-only REST + WebSocket client**
- `meta()` — perpetuals universe (names, `szDecimals`, …)
- `spot_meta()` — spot token list and spot universe
- `all_mids()` — all mid prices as `{coin: priceString}`
- `l2_snapshot(coin)` — full L2 order-book snapshot
- `bbo(coin)` — best bid/offer (WebSocket-only; REST returns 422 on testnet)
- `candle_snapshot(coin, interval, start_ms[, end_ms])` — OHLCV candles; intervals: `1m 5m 15m 30m 1h 4h 8h 12h 1d 3d 1w`
- `perp_asset_ctxs()` — funding rate, open interest, and mark price for all perps
- `spot_asset_ctxs()` — spot market context
- `funding_history(coin, start_ms[, end_ms])` — historical funding rates
- `user_state(address)` — margin summary and open positions
- `open_orders(address)` — open orders
- `user_fills(address)` — complete fill history
- `user_fills_by_time(address, start_ms[, end_ms])` — fills within a time window
- `query_order_by_oid(address, oid)` — single order lookup by order ID
- `query_order_by_cloid(address, cloid)` — single order lookup by client order ID
- `sub_accounts(address)` — sub-account list
- `load_meta()` — populates `coin_to_asset` map (called automatically by `Exchange`)
- `subscribe(subscription, callback)` — WebSocket subscription; returns integer ID; multiple callbacks per channel supported
- `unsubscribe(subscription, id)` — remove a subscription; double-unsubscribe is harmless
- `skip_ws` constructor flag (`Info(url, true)`) — disables WebSocket for REST-only use

**`Exchange` — authenticated trading client**
- Order management
  - `order(coin, is_buy, sz, limit_px, order_type[, reduce_only, cloid, builder, grouping])` — single limit or trigger order
  - `bulk_orders(orders[, builder, grouping])` — multiple orders in one round trip
  - `market_open(coin, is_buy, sz[, slippage, cloid])` — IoC limit at mid ± slippage (default 5 %)
  - `market_close(coin[, sz, slippage, cloid])` — close position at mid ± slippage; queries `user_state` for size
- Cancel
  - `cancel(coin, oid)` / `cancel_by_cloid(coin, cloid)`
  - `bulk_cancel(cancels)` / `bulk_cancel_by_cloid(cancels)`
  - `schedule_cancel([time_ms])` — dead-man's switch
- Modify
  - `modify_order(oid, new_order)` / `bulk_modify_orders(mods)`
- Leverage and margin
  - `update_leverage(coin, is_cross, leverage)`
  - `update_isolated_margin(coin, is_buy, ntl)`
- Transfers (user-signed EIP-712)
  - `usd_class_transfer(amount, to_perp)` — spot ↔ perp vault
  - `usd_transfer(amount, destination)` — send USDC to an address
  - `spot_transfer(amount, destination, token)` — send spot tokens
  - `withdraw_from_bridge(amount, destination)` — L1 withdrawal
- Agent and builder
  - `approve_agent(agent_address[, agent_name])`
  - `approve_builder_fee(builder, max_fee_rate)`
- Sub-accounts
  - `create_sub_account(name)`
  - `sub_account_transfer(usd, to_sub, sub_account_user)`
- `wallet_address()` — returns the Ethereum address derived from the private key
- `vault_address` constructor parameter — sign on behalf of a sub-account / vault

**Cryptographic internals**
- Embedded Keccak-256 implementation (Ethereum's pre-FIPS variant — distinct from OpenSSL's `SHA3-256`)
- EIP-712 signing for L1 actions (Exchange domain, `chainId` 1337)
- EIP-712 signing for user-signed actions (HyperliquidSignTransaction domain, `chainId` 0x66eee)
- msgpack-based action hashing using `nlohmann::ordered_json` for byte-compatible key ordering with the Python SDK
- Private key accepted with or without `"0x"` prefix

**Types (`hyperliquid::utils::types`)**
- `Tif` enum — `Gtc`, `Ioc`, `Alo`
- `LimitOrderType{tif}` — standard limit order
- `TriggerOrderType{trigger_px, is_market, tpsl}` — stop / TP-SL; `tpsl` is `"tp"` or `"sl"`
- `Cloid` — 16-byte client order ID; construct via `Cloid::from_int(uint64_t)` or `Cloid::from_str("0x...")`
- `BuilderInfo{builder_address, fee_tbps}` — `fee_tbps` is in tenths of basis points (e.g. `10` = 1 bps)
- `OrderRequest`, `CancelRequest`, `CancelByCloidRequest`, `ModifyRequest` — bulk operation helpers

**Constants**
- `MAINNET_API_URL` — `https://api.hyperliquid.xyz`
- `TESTNET_API_URL` — `https://api.hyperliquid-testnet.xyz`

#### Build system
- CMake ≥ 3.20 build with C++20
- vcpkg integration (`nlohmann-json`, `openssl`, `gtest`, `slick-net`)
- Targets: `hyperliquid` (static library), `basic_order` (example), `hyperliquid_tests` (unit tests), `hyperliquid_integration_tests` (integration tests)

#### Examples
- `examples/basic_order.cpp` — places a resting GTC limit buy on testnet then cancels it; accepts private key as an optional CLI argument, falls back to the hardhat test key

#### Tests
- Unit tests (`hyperliquid_tests`, ~40 tests, no network):
  - Keccak-256 hash vectors
  - EIP-712 signing correctness
  - Type serialisation (`Tif`, `Cloid`, `BuilderInfo`, order/cancel request encoding)
  - WebSocket URL construction and channel-identifier utilities
- Integration tests (`hyperliquid_integration_tests`, ~54 tests, require testnet access):
  - REST: all `Info` endpoints against `api.hyperliquid-testnet.xyz`
  - WebSocket: live subscription tests for `allMids`, `l2Book`, `trades`, `candle`; multi-callback fan-out; partial unsubscribe; unsubscribe-stops-delivery

[0.1.0]: https://github.com/your-org/hyperliquid-cpp/releases/tag/v0.1.0
