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
#include <slick/stream_buffer_multiplexer.hpp>

namespace hyperliquid {

// Read-only Hyperliquid API client.
// Mirrors the Python SDK's Info class.
// All REST methods throw Api::HttpError on HTTP 4xx/5xx and std::runtime_error
// on network failure or malformed JSON response.
class Info : public Api {
public:
    explicit Info(
        std::string_view base_url,
        bool skip_ws = false,
        bool user_thread_dispatch = false,             // if true, callbacks fire on dispatch() caller thread
        uint32_t mux_record_size = 1u << 18,           // 256K message records
        const char* mux_shm_name = nullptr,            // default not using shared memory
        uint32_t read_buffer_size = 1u << 24,          // 16 MB reading buffer
        uint32_t read_control_size = 1u << 16,         // 64K control size
        const char* read_buffer_shm_name = nullptr,    // default not using shared memory
        uint32_t write_buffer_size = 1u << 20          // 1 MB writing buffer
    );

    explicit Info(
        std::string_view base_url,
        slick::stream_buffer_multiplexer &mux,
        bool skip_ws = false,
        bool user_thread_dispatch = false,             // if true, callbacks fire on dispatch() caller thread
        uint32_t read_buffer_size = 1u << 24,          // 16 MB reading buffer
        uint32_t read_control_size = 1u << 16,         // 64K control size
        const char* read_buffer_shm_name = nullptr,    // default not using shared memory
        uint32_t write_buffer_size = 1u << 20         // 1 MB writing buffer
    );


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
    // Throws std::runtime_error if the WebSocket was not started (skip_ws=true).
    // Throws std::runtime_error if the channel does not support multiple subscriptions.
    int subscribe(const nlohmann::json& subscription,
                  std::function<void(const nlohmann::json&)> callback);

    // Unsubscribe by subscription_id from subscribe().
    void unsubscribe(const nlohmann::json& subscription, int subscription_id);

    WebsocketManager* websocket() noexcept {
        return ws_.get();
    }

    uint32_t ws_producer_id() const noexcept {
        return ws_ ? ws_->producer_id() : WebsocketManager::INVALID_PRODUCER_ID;
    }

    // Scan at most max_count queued records and invoke callbacks for records
    // belonging to this Info's WebSocket on the calling thread. Only meaningful
    // when Info was constructed with user_thread_dispatch=true; otherwise
    // returns 0.
    size_t dispatch(std::size_t max_count = 100) {
        return ws_ ? ws_->dispatch(max_count) : 0;
    }

    // Dispatch the message to their registered callbacks on the calling thread.
    // Only meaningful when Info was constructed with user_thread_dispatch=true; otherwise returns false.
    bool dispatch(uint32_t producer_id, const char* data, std::size_t length);

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
    // Throws std::invalid_argument if the coin is not found in the loaded metadata.
    int name_to_asset(std::string_view coin_or_name);

    // Ensure all asset lookup maps are populated (call before using Exchange).
    // Throws Api::HttpError or std::runtime_error on network/HTTP failure.
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
