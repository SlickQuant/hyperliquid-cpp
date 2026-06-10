#pragma once

#include <hyperliquid/api.hpp>
#include <hyperliquid/websocket_manager.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace hyperliquid {

// Read-only Hyperliquid API client.
// Mirrors the Python SDK's Info class.
class Info : public Api {
public:
    explicit Info(std::string_view base_url, bool skip_ws = false);

    // ── Market / exchange metadata ────────────────────────────────────────────

    // Perpetuals universe metadata (array of {name, szDecimals} per asset).
    nlohmann::json meta();

    // Spot market metadata.
    nlohmann::json spot_meta();

    // All perpetuals mid prices as {"BTC": "...", "ETH": "..."}.
    nlohmann::json all_mids();

    // L2 order book snapshot for one coin.
    nlohmann::json l2_snapshot(std::string_view coin);

    // Best bid/offer for one coin.
    nlohmann::json bbo(std::string_view coin);

    // Candlestick data: interval e.g. "1m","5m","1h","1d".
    nlohmann::json candle_snapshot(std::string_view coin, std::string_view interval,
                                    int64_t start_ms,
                                    std::optional<int64_t> end_ms = {});

    // Perpetuals asset context list (funding, OI, mark price, …).
    nlohmann::json perp_asset_ctxs();

    // Spot asset context list.
    nlohmann::json spot_asset_ctxs();

    // Funding rate history for a coin.
    nlohmann::json funding_history(std::string_view coin, int64_t start_ms,
                                    std::optional<int64_t> end_ms = {});

    // ── User account data ─────────────────────────────────────────────────────

    // Full account state: margin summary, positions, open orders.
    nlohmann::json user_state(std::string_view address);

    // Open orders for a user.
    nlohmann::json open_orders(std::string_view address);

    // Historical fills for a user.
    nlohmann::json user_fills(std::string_view address);

    // Fills since a given timestamp.
    nlohmann::json user_fills_by_time(std::string_view address,
                                       int64_t start_ms,
                                       std::optional<int64_t> end_ms = {});

    // Query a single order by oid.
    nlohmann::json query_order_by_oid(std::string_view address, int64_t oid);

    // Query a single order by client order ID.
    nlohmann::json query_order_by_cloid(std::string_view address,
                                         std::string_view cloid);

    // Sub-account list for an address.
    nlohmann::json sub_accounts(std::string_view address);

    // ── WebSocket subscriptions ───────────────────────────────────────────────

    // Subscribe to a channel. Returns a subscription_id for unsubscribe.
    // Use the same JSON format as the Python SDK:
    //   {"type": "l2Book", "coin": "ETH"}
    //   {"type": "userFills", "user": "0x..."}
    int subscribe(const nlohmann::json& subscription,
                  std::function<void(const nlohmann::json&)> callback);

    // Unsubscribe by subscription_id from subscribe().
    void unsubscribe(const nlohmann::json& subscription, int subscription_id);

    // ── Asset index lookup ────────────────────────────────────────────────────

    // Canonical coin string -> asset index. Perp assets are 0-based; spot assets
    // use 10000 + spot index to match the official Python SDK.
    std::unordered_map<std::string, int> coin_to_asset;

    // Human-readable name -> canonical coin string (e.g. "PURR/USDC" -> "@0").
    std::unordered_map<std::string, std::string> name_to_coin;

    // Asset index -> size decimals.
    std::unordered_map<int, int> asset_to_sz_decimals;

    // Resolve a human-readable market name or canonical coin string to the wire coin.
    std::string canonical_coin(std::string_view coin_or_name);

    // Resolve a human-readable market name or canonical coin string to an asset id.
    int name_to_asset(std::string_view coin_or_name);

    // Ensure all asset lookup maps are populated (call before using Exchange).
    void load_meta();

private:
    nlohmann::json info_post(const nlohmann::json& payload);
    void populate_perp_meta(const nlohmann::json& meta);
    void populate_spot_meta(const nlohmann::json& spot_meta);
    nlohmann::json remap_subscription(const nlohmann::json& subscription);

    bool perp_meta_loaded_ = false;
    bool spot_meta_loaded_ = false;
    std::unique_ptr<WebsocketManager> ws_;
};

} // namespace hyperliquid
