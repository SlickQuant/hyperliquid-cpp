#include <hyperliquid/info.hpp>

#include <stdexcept>
#include <utility>

namespace hyperliquid {

Info::Info(std::string_view base_url, bool skip_ws)
    : Api(base_url)
{
    if (!skip_ws)
        ws_ = std::make_unique<WebsocketManager>(base_url);
}

// Internal helper

nlohmann::json Info::info_post(const nlohmann::json& payload) {
    return post("/info", payload);
}

nlohmann::json Info::remap_subscription(const nlohmann::json& subscription) {
    auto remapped = subscription;
    const auto type = remapped.value("type", "");
    if (type == "l2Book" || type == "trades" || type == "candle" ||
        type == "bbo" || type == "activeAssetCtx" || type == "activeAssetData")
    {
        remapped["coin"] = canonical_coin(remapped.at("coin").get<std::string>());
    }
    return remapped;
}

void Info::populate_perp_meta(const nlohmann::json& meta) {
    int idx = 0;
    for (const auto& asset : meta.at("universe")) {
        const std::string coin = asset.at("name").get<std::string>();
        coin_to_asset[coin] = idx;
        name_to_coin[coin] = coin;
        asset_to_sz_decimals[idx] = asset.at("szDecimals").get<int>();
        ++idx;
    }
    perp_meta_loaded_ = true;
}

void Info::populate_spot_meta(const nlohmann::json& spot_meta) {
    std::unordered_map<int, nlohmann::json> token_by_index;
    for (const auto& token : spot_meta.at("tokens"))
        token_by_index[token.at("index").get<int>()] = token;

    for (const auto& spot_info : spot_meta.at("universe")) {
        const int index = spot_info.at("index").get<int>();
        const int asset = 10000 + index;
        const std::string coin = spot_info.at("name").get<std::string>();

        coin_to_asset[coin] = asset;
        name_to_coin[coin] = coin;
        name_to_coin["@" + std::to_string(index)] = coin;

        const int base = spot_info.at("tokens").at(0).get<int>();
        const int quote = spot_info.at("tokens").at(1).get<int>();
        const auto& base_info = token_by_index.at(base);
        const auto& quote_info = token_by_index.at(quote);

        asset_to_sz_decimals[asset] = base_info.at("szDecimals").get<int>();

        const std::string pair_name =
            base_info.at("name").get<std::string>() + "/" +
            quote_info.at("name").get<std::string>();
        name_to_coin.emplace(pair_name, coin);
    }

    spot_meta_loaded_ = true;
}

// Meta

void Info::load_meta() {
    if (!spot_meta_loaded_)
        spot_meta();
    if (!perp_meta_loaded_)
        meta();
}

nlohmann::json Info::meta() {
    auto result = info_post({{"type", "meta"}});
    if (!perp_meta_loaded_)
        populate_perp_meta(result);
    return result;
}

nlohmann::json Info::spot_meta() {
    auto result = info_post({{"type", "spotMeta"}});
    if (!spot_meta_loaded_)
        populate_spot_meta(result);
    return result;
}

std::string Info::canonical_coin(std::string_view coin_or_name) {
    if (!perp_meta_loaded_ || !spot_meta_loaded_)
        load_meta();

    const auto it = name_to_coin.find(std::string(coin_or_name));
    if (it != name_to_coin.end())
        return it->second;
    return std::string(coin_or_name);
}

int Info::name_to_asset(std::string_view coin_or_name) {
    const std::string coin = canonical_coin(coin_or_name);
    const auto it = coin_to_asset.find(coin);
    if (it == coin_to_asset.end())
        throw std::invalid_argument("Unknown coin: " + std::string(coin_or_name));
    return it->second;
}

// Market data

nlohmann::json Info::all_mids() {
    return info_post({{"type", "allMids"}});
}

nlohmann::json Info::l2_snapshot(std::string_view coin) {
    return info_post({{"type", "l2Book"}, {"coin", canonical_coin(coin)}});
}

nlohmann::json Info::bbo(std::string_view coin) {
    return info_post({{"type", "bbo"}, {"coin", canonical_coin(coin)}});
}

nlohmann::json Info::candle_snapshot(
    std::string_view coin, std::string_view interval,
    int64_t start_ms, std::optional<int64_t> end_ms)
{
    nlohmann::json req{
        {"type", "candleSnapshot"},
        {"req",
         {
             {"coin", canonical_coin(coin)},
             {"interval", interval},
             {"startTime", start_ms},
         }},
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
        {"type", "fundingHistory"},
        {"coin", canonical_coin(coin)},
        {"startTime", start_ms},
    };
    if (end_ms)
        req["endTime"] = *end_ms;
    return info_post(req);
}

// User data

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
        {"type", "userFillsByTime"},
        {"user", address},
        {"startTime", start_ms},
    };
    if (end_ms)
        req["endTime"] = *end_ms;
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

// WebSocket

int Info::subscribe(const nlohmann::json& subscription,
                    std::function<void(const nlohmann::json&)> callback)
{
    if (!ws_)
        throw std::runtime_error("WebSocket not started (use skip_ws=false)");
    return ws_->subscribe(remap_subscription(subscription), std::move(callback));
}

void Info::unsubscribe(const nlohmann::json& subscription, int subscription_id) {
    if (ws_)
        ws_->unsubscribe(remap_subscription(subscription), subscription_id);
}

} // namespace hyperliquid
