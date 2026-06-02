#include <hyperliquid/info.hpp>

namespace hyperliquid {

Info::Info(std::string_view base_url, bool skip_ws)
    : Api(base_url)
{
    if (!skip_ws)
        ws_ = std::make_unique<WebsocketManager>(base_url);
}

// ── Internal helper ────────────────────────────────────────────────────────────

nlohmann::json Info::info_post(const nlohmann::json& payload) {
    return post("/info", payload);
}

// ── Meta ────────────────────────────────────────────────────────────────────────

void Info::load_meta() {
    if (meta_loaded_) return;
    auto m = info_post({{"type", "meta"}});
    int idx = 0;
    for (const auto& asset : m["universe"]) {
        coin_to_asset[asset["name"].get<std::string>()] = idx++;
    }
    meta_loaded_ = true;
}

nlohmann::json Info::meta() {
    auto result = info_post({{"type", "meta"}});
    // Populate coin_to_asset as a side-effect
    if (!meta_loaded_) {
        int idx = 0;
        for (const auto& asset : result["universe"]) {
            coin_to_asset[asset["name"].get<std::string>()] = idx++;
        }
        meta_loaded_ = true;
    }
    return result;
}

nlohmann::json Info::spot_meta() {
    return info_post({{"type", "spotMeta"}});
}

// ── Market data ────────────────────────────────────────────────────────────────

nlohmann::json Info::all_mids() {
    return info_post({{"type", "allMids"}});
}

nlohmann::json Info::l2_snapshot(std::string_view coin) {
    return info_post({{"type", "l2Book"}, {"coin", coin}});
}

nlohmann::json Info::bbo(std::string_view coin) {
    return info_post({{"type", "bbo"}, {"coin", coin}});
}

nlohmann::json Info::candle_snapshot(
    std::string_view coin, std::string_view interval,
    int64_t start_ms, std::optional<int64_t> end_ms)
{
    nlohmann::json req{
        {"type",      "candleSnapshot"},
        {"req", {
            {"coin",      coin},
            {"interval",  interval},
            {"startTime", start_ms},
        }}
    };
    if (end_ms)
        req["req"]["endTime"] = *end_ms;
    return info_post(req);
}

nlohmann::json Info::perp_asset_ctxs() {
    return info_post({{"type", "metaAndAssetCtxs"}});
}

nlohmann::json Info::spot_asset_ctxs() {
    return info_post({{"type", "spotMetaAndAssetCtxs"}});
}

nlohmann::json Info::funding_history(
    std::string_view coin, int64_t start_ms, std::optional<int64_t> end_ms)
{
    nlohmann::json req{
        {"type",      "fundingHistory"},
        {"coin",      coin},
        {"startTime", start_ms},
    };
    if (end_ms) req["endTime"] = *end_ms;
    return info_post(req);
}

// ── User data ──────────────────────────────────────────────────────────────────

nlohmann::json Info::user_state(std::string_view address) {
    return info_post({{"type", "clearinghouseState"}, {"user", address}});
}

nlohmann::json Info::open_orders(std::string_view address) {
    return info_post({{"type", "openOrders"}, {"user", address}});
}

nlohmann::json Info::user_fills(std::string_view address) {
    return info_post({{"type", "userFills"}, {"user", address}});
}

nlohmann::json Info::user_fills_by_time(
    std::string_view address, int64_t start_ms, std::optional<int64_t> end_ms)
{
    nlohmann::json req{
        {"type",      "userFillsByTime"},
        {"user",      address},
        {"startTime", start_ms},
    };
    if (end_ms) req["endTime"] = *end_ms;
    return info_post(req);
}

nlohmann::json Info::query_order_by_oid(std::string_view address, int64_t oid) {
    return info_post({{"type", "orderStatus"}, {"user", address}, {"oid", oid}});
}

nlohmann::json Info::query_order_by_cloid(
    std::string_view address, std::string_view cloid)
{
    return info_post({{"type", "orderStatus"}, {"user", address}, {"oid", cloid}});
}

nlohmann::json Info::sub_accounts(std::string_view address) {
    return info_post({{"type", "subAccounts"}, {"user", address}});
}

// ── WebSocket ──────────────────────────────────────────────────────────────────

int Info::subscribe(const nlohmann::json& subscription,
                     std::function<void(const nlohmann::json&)> callback)
{
    if (!ws_)
        throw std::runtime_error("WebSocket not started (use skip_ws=false)");
    return ws_->subscribe(subscription, std::move(callback));
}

void Info::unsubscribe(const nlohmann::json& subscription, int subscription_id) {
    if (ws_)
        ws_->unsubscribe(subscription, subscription_id);
}

} // namespace hyperliquid
