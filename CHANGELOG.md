# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.4.0] - 2026-10-02

### Changed
- Require `slick-net` 4.0.0 (was 3.0.0) in `find_package`, the `FetchContent` fallback tag, and the installed `hyperliquid-config.cmake` `find_dependency`.
- Exceptions thrown by subscription callbacks are now logged and swallowed instead of propagating. This includes `Info::dispatch()` / `WebsocketManager::dispatch()` in user-thread dispatch mode, which no longer rethrow callback exceptions.
- Inputs that were previously accepted and signed silently are now rejected (see Fixed): malformed `Cloid` values, non-hex characters in hex inputs, EIP-712 `address` fields that are not 20 bytes, and malformed or out-of-range private keys.

### Fixed
- A throwing subscription callback or a malformed routed message (a known channel with a missing field) stopped the WebSocket receive loop without triggering a reconnect. Routing and each callback are now isolated with logging, so the other callbacks still receive the message and the receive loop keeps running.
- Use-after-free when a `WebsocketManager` was destroyed while a socket callback was still running. Socket callbacks now enter a lock-free lifetime gate held by `shared_ptr`, and the destructor waits for callbacks already running to finish before releasing state. Destroying the manager from inside its own callback does not deadlock.
- The `WebsocketManager` destructor now joins the ping thread before detaching the socket, so a reconnect `open()` from the ping thread can no longer race the shutdown.
- Reconnect replay now continues with the remaining subscriptions when resending one of them fails.
- A mainnet URL with a trailing slash (e.g. `https://api.hyperliquid.xyz/`) sent requests to mainnet but signed them for testnet (`hyperliquidChain: "Testnet"`). `Exchange` now detects mainnet from the normalized base URL.
- `Cloid` accepted non-hex characters as long as the prefix and length were correct.
- `hex_to_bytes()` ignored failed `sscanf` conversions (and accepted signs and spaces), so an invalid vault address could be encoded as zero bytes and signed. It now throws `std::invalid_argument` on any non-hex character.
- EIP-712 `address` fields of the wrong length were silently padded or truncated to 20 bytes and signed; they now throw `std::invalid_argument`.
- Private keys were parsed with `BN_hex2bn`, which stops at the first non-hex character, so a malformed key loaded as a different key and signed with the wrong wallet. Keys must now be exactly 64 hex digits (optional `0x` prefix) within the secp256k1 range `[1, n-1]`; otherwise `std::runtime_error` is thrown.

### Tests
- `ServiceThreadDispatch.ThrowingCallbackAndMalformedMessageKeepReceiveLoopAlive` — a malformed `l2Book` message and a throwing callback do not stop delivery of subsequent messages on the socket thread.
- `ServiceThreadDispatch.DestructorWaitsForInFlightCallback` — destroying the manager does not complete while a socket callback is still running.
- `UserThreadDispatch.ThrowingCallbackDoesNotStopOtherCallbacksOrDispatch` and `UserThreadDispatch.MalformedRoutedMessageIsDropped` — the same isolation in user-thread dispatch mode.
- `ExchangeNetwork.MainnetUrlWithTrailingSlashSignsForMainnet` — a mainnet URL with trailing slashes signs transfers with `hyperliquidChain: "Mainnet"`.
- `Cloid.NonHexCharacterThrows`, `Cloid.UpperCaseHexAccepted`, `HexUtils.NonHexCharacterThrows`, `HexUtils.InvalidVaultAddressRejectedBySigning`, `SignUserSignedAction.AddressFieldMustBe20Bytes` — hex and address validation.
- `PrivateKeyToAddress.MalformedKeyThrows`, `PrivateKeyToAddress.OutOfRangeKeyThrows`, `PrivateKeyToAddress.MaxValidKeyAccepted` — private key format and range validation.
- The in-process test WebSocket server can now push scripted messages after each subscribe.

## [0.3.0] - 2026-07-07

### Added
- `hyperliquid::decode_l2_diff(std::string_view)` — decodes the compact `data.c` binary payload from Hyperliquid's undocumented `l2` WebSocket channel into a `nlohmann::json` diff object. The payload is standard base64 → raw deflate; decoding uses `EVP_DecodeBlock` (OpenSSL, already linked) and zlib `inflate`.
- ZLIB added as an explicit CMake dependency (`find_package(ZLIB REQUIRED)` / `ZLIB::ZLIB`); propagated to installed consumers via `find_dependency(ZLIB)` in `hyperliquid-config.cmake`.

### Changed
- `market_data_websocket` example now connects to `MAINNET_API_URL` instead of `TESTNET_API_URL`.
- Suppress ping message logging

### Fixed
- Duplicate wire subscribe when `subscribe()` races connection establishment: the subscriber thread and the `on_connected()` replay could both send the subscribe message. Wire sends are now gated by a lock-free connection-epoch CAS so exactly one send occurs per subscription per connection; the final unsubscribe releases the claim so a re-subscribe on the same connection sends again.

### Tests
- `L2DiffDecoder.DecodesCompressedDiffPayload` — round-trip decode of a captured mainnet `l2` channel payload; asserts coin, timestamp, bid/ask levels, and removed-level arrays.
- `L2DiffDecoder.RejectsInvalidBase64` — invalid base64 characters throw `std::invalid_argument`.
- `L2DiffDecoder.RejectsInvalidDeflateData` — valid base64 that is not raw deflate throws `std::runtime_error`.
- `L2DiffDecoder.RejectsNonJsonDeflatePayload` — valid raw deflate that decompresses to non-JSON throws `std::runtime_error`.

## [0.2.1] - 2026-06-23

### Added
- Add automatic `WebsocketManager` reconnect attempts with bounded exponential backoff after disconnects or transport errors.
- Replay active WebSocket subscriptions after reconnect so `Info::subscribe()` callbacks resume without manual re-subscription.

### Fixed
- Keep WebSocket subscription replay lock-free and synchronized with unsubscribe by tracking per-identifier subscription state, in-flight subscribe/replay sends, and final unsubscribe phases with atomics.
- Avoid stale server-side subscriptions when a reconnect replay overlaps with the last local `unsubscribe()` for a channel.
- Coalesce multiple callbacks on the same channel into one wire subscription until the last callback unsubscribes.

### Tests
- Add offline `SubscriptionTracking` tests covering subscribe while disconnected, callback routing after unsubscribe, concurrent subscribe/unsubscribe, wire subscription coalescing, and unsubscribe-before-resubscribe ordering using a local in-process WebSocket server.

## [0.2.0] -2026-06-19

### Added
- Add a `market_data_websocket` example that subscribes to public testnet `allMids` and per-coin `l2Book` WebSocket updates for one or more coins.
- Add a `market_data_websocket_per_coin` example that subscribes to ETH and BTC market data using one WebSocket connection per coin.
- Add user-thread WebSocket dispatch support through `Info::dispatch()`, `WebsocketManager::dispatch()`, and `user_thread_dispatch` constructor options.
- Add `Info` and `WebsocketManager` constructors that accept an external `slick::stream_buffer_multiplexer` for shared dispatch queues.
- Add `market_data_websocket_user_thread_dispatch`, `market_data_websocket_per_coin_user_thread_dispatch`, and `market_data_websocket_shm_reader` examples.
- Add shared-memory WebSocket buffer configuration options so external readers can attach to the market-data stream.

### Changed
- Normalize line ending to LF
- Update the WebSocket transport integration for `slick-net` 3.0.0 and the slick dynamic-buffer stream-buffer-multiplexer backend.
- Move example target registration into `examples/CMakeLists.txt`.
- Publish `market_data_websocket` records to named shared-memory segments that `market_data_websocket_shm_reader` can open.
- Move test CMake setup into `tests/CMakeLists.txt`, register GoogleTest cases with `gtest_discover_tests()`, and keep CI integration tests running as a single executable to avoid repeated testnet setup per discovered test case.
- Defer GoogleTest discovery to CTest with a longer discovery timeout so test executables are not run during the build.
- Stabilize the WebSocket partial-unsubscribe integration test by waiting for the next `allMids` event instead of assuming one arrives within a fixed one-second sleep.

### Tests
- Add offline regression tests for user-thread WebSocket dispatch routing, foreign producer filtering, and shared-memory writer/reader attachment.

## [0.1.2] - 2026-06-09

### Added
- **`Exchange`: API-wallet account selection support** — added a constructor overload with optional `account_address` so authenticated flows can query the effective trading account separately from the signing key.
- **`Info`: canonical asset lookup helpers** — added `name_to_coin`, `name_to_asset`, `asset_to_sz_decimals`, and `canonical_coin()` to resolve human-readable market names to Hyperliquid wire identifiers and asset ids.
- **`WebsocketManager::message_to_identifier()`** — explicit inbound message routing helper for channel-to-handler lookup.
- **Offline exchange parity tests** — added `tests/test_exchange.cpp` covering spot/perp mapping, payload shapes, price rounding, and effective-account selection.

### Fixed
- **`WebsocketManager`: use-after-free / same-dispatch unsubscribe race** — callbacks now dispatch from a lock-free snapshot of shared handler state instead of copying `std::function`s under a mutex. Each callback entry carries an `active` flag and `in_flight` counter: `unsubscribe()` removes the callback from future snapshots, marks that specific callback inactive, and waits on its per-callback in-flight count with C++20 atomic wait/notify. This prevents a later callback from firing after being removed earlier in the same snapshot while still allowing self-unsubscribe without deadlocking.
- **`updateIsolatedMargin` payload shape** — exchange requests now send the documented `ntli` field with `isBuy: true` instead of the previous mismatched parameters.
- **`usdClassTransfer` formatting** — the signed action now uses the SDK-compatible `"amount"` string and appends `" subaccount:<address>"` when trading through a vault.
- **Effective account selection for `market_close()`** — account state is now queried against `vault_address`, then `account_address`, then the signer wallet, matching Hyperliquid API-wallet semantics.
- **Vault handling for user-signed transfers** — user-signed exchange payloads now include or omit `vaultAddress` consistently with the current Python SDK behavior.
- **`basic_order.cpp` default credentials** — the example now uses the Hardhat private key instead of the Hardhat address.

### Changed
- **`WebsocketManager`: removed `pending_subs_` pre-connection queue** — the underlying `slick::net::Websocket` already buffers outbound frames until the connection is established, making the manual pending-subscription queue redundant. `subscribe()` and `unsubscribe()` now call `ws_->send()` unconditionally; `flush_pending()`, `writer_mutex_`, and the `pending_subs_` vector are all removed.
- **Metadata loading now matches the Hyperliquid SDK/docs** — perp assets use `meta` indices, spot assets use `10000 + spotMeta.index`, and spot aliases such as `PURR/USDC` and `@<index>` resolve to canonical wire coins.
- **REST and WebSocket market-data requests now canonicalize coin names** before sending requests, so spot and perp subscriptions/queries use documented wire names instead of assuming perpetual-only symbols.
- **Exchange precision handling was tightened to SDK behavior** — `float_to_wire()` and `float_to_usd_int()` now reject silent rounding, and market-order slippage pricing now applies 5 significant figures with max decimals derived from `szDecimals`.
- **WebSocket routing was rebuilt around explicit message identifiers** — `trades`, `candle`, `activeAssetCtx`, `activeSpotAssetCtx`, `userEvents`, and `orderUpdates` now route using the actual inbound payload shape, with a 50-second heartbeat under the documented idle timeout.
- **Internal identifier normalization is narrower** — coin-bearing websocket identifiers now preserve canonical coin casing, while address-bearing identifiers still lowercase the user/address portion for stable routing.
- **Docs and examples were refreshed** — `README.md` now documents the expanded asset lookup maps, `account_address`, and the updated `update_isolated_margin(coin, amount)` signature.

### Tests
- **`UnsubscribeBlocksUntilCallbackCompletes`** (integration) — regression test for the above fix; subscribes with a callback that blocks until explicitly released, calls `unsubscribe()` concurrently from a second thread, and asserts that `unsubscribe()` does not return before the in-flight callback has finished.
- **`CallbackCanRemoveLaterCallbackFromSameDispatch`** (integration) — regression test for the snapshot hazard where callback A unsubscribes callback B on the same channel before the dispatcher reaches B in the already-loaded snapshot.
- **Signing regressions** — added tests asserting that `float_to_wire()` and `float_to_usd_int()` throw when serialization would require rounding.
- **WebSocket routing regressions** — extended channel-identifier tests to cover inbound routing, snapshot-safe channel naming, spot/perp active-asset messages, address normalization, and preserved coin casing.

## [0.1.1] - 2026-06-08

### Added
- `install()` rules and generated CMake package-config files (`hyperliquid-config.cmake`, `hyperliquid-config-version.cmake`, `hyperliquid-targets.cmake`) so the library can be consumed via `find_package(hyperliquid CONFIG REQUIRED)` after `cmake --install` / `vcpkg install`
- `HYPERLIQUID_BUILD_TESTS` and `HYPERLIQUID_BUILD_EXAMPLES` CMake options — default `ON` for top-level builds, automatically `OFF` when the project is consumed via `add_subdirectory`/`FetchContent`

### Changed
- `target_compile_features(hyperliquid PUBLIC cxx_std_20)` — the C++20 requirement now propagates to consumers through the exported target
- Raised minimum CMake version from 3.20 to 3.21 (required for `PROJECT_IS_TOP_LEVEL`)

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
