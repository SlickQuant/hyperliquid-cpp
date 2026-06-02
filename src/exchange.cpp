#include <hyperliquid/exchange.hpp>
#include <hyperliquid/utils/signing.hpp>
#include <hyperliquid/utils/constants.hpp>

#include <cmath>
#include <stdexcept>

namespace hyperliquid {

using signing::sign_l1_action;
using signing::sign_user_signed_action;
using signing::get_timestamp_ms;
using signing::private_key_to_address;
using signing::float_to_wire;
using signing::float_to_usd_int;
using signing::order_request_to_wire;
using signing::order_wires_to_action;

Exchange::Exchange(
    std::string private_key_hex,
    std::string_view base_url,
    std::shared_ptr<Info> info,
    std::optional<std::string> vault_address)
    : Api(base_url)
    , private_key_(std::move(private_key_hex))
    , vault_address_(std::move(vault_address))
    , info_(std::move(info))
    , is_mainnet_(base_url == MAINNET_API_URL)
{
    wallet_ = private_key_to_address(private_key_);
    info_->load_meta();
}

// ── Helpers ───────────────────────────────────────────────────────────────────

int Exchange::asset_index(std::string_view coin) {
    auto it = info_->coin_to_asset.find(std::string(coin));
    if (it == info_->coin_to_asset.end())
        throw std::invalid_argument(std::string("Unknown coin: ") + std::string(coin));
    return it->second;
}

nlohmann::json Exchange::post_action(
    nlohmann::ordered_json action,
    std::optional<std::string> vault_override)
{
    int64_t nonce = get_timestamp_ms();
    const auto& va = vault_override ? vault_override : vault_address_;

    auto sig = sign_l1_action(private_key_, action, va, nonce, is_mainnet_);

    nlohmann::json req;
    req["action"]    = action;
    req["nonce"]     = nonce;
    req["signature"] = {{"r", sig.r}, {"s", sig.s}, {"v", sig.v}};
    if (va) req["vaultAddress"] = *va;
    else    req["vaultAddress"] = nullptr;

    return post("/exchange", req);
}

nlohmann::json Exchange::post_user_signed_action(
    nlohmann::ordered_json action,
    const std::vector<std::pair<std::string, std::string>>& payload_types,
    std::string_view primary_type)
{
    int64_t nonce = get_timestamp_ms();

    // Some EIP-712 action types name the timestamp field "time" (usdSend, spotSend,
    // withdraw3); others name it "nonce" (usdClassTransfer, approveAgent, …).
    // Use whichever name the caller declared in payload_types.
    const char* ts_key = "nonce";
    for (const auto& [fname, ftype] : payload_types)
        if (fname == "time") { ts_key = "time"; break; }
    action[ts_key] = nonce;

    auto sig = sign_user_signed_action(
        private_key_, action, payload_types, primary_type, is_mainnet_);

    nlohmann::json req;
    req["action"]    = action;
    req["nonce"]     = nonce;
    req["signature"] = {{"r", sig.r}, {"s", sig.s}, {"v", sig.v}};
    req["vaultAddress"] = nullptr;

    return post("/exchange", req);
}

double Exchange::slippage_price(std::string_view coin, bool is_buy, double slippage) {
    auto mids = info_->all_mids();
    double mid = std::stod(mids.at(std::string(coin)).get<std::string>());
    return is_buy ? mid * (1.0 + slippage) : mid * (1.0 - slippage);
}

// ── Order management ──────────────────────────────────────────────────────────

nlohmann::json Exchange::order(
    std::string_view coin, bool is_buy,
    double sz, double limit_px,
    const OrderType& order_type,
    bool reduce_only,
    std::optional<Cloid> cloid,
    std::optional<BuilderInfo> builder,
    const std::string& grouping)
{
    OrderRequest req;
    req.coin        = coin;
    req.is_buy      = is_buy;
    req.sz          = sz;
    req.limit_px    = limit_px;
    req.order_type  = order_type;
    req.reduce_only = reduce_only;
    req.cloid       = std::move(cloid);

    return bulk_orders({std::move(req)}, std::move(builder), grouping);
}

nlohmann::json Exchange::bulk_orders(
    std::vector<OrderRequest> orders,
    std::optional<BuilderInfo> builder,
    const std::string& grouping)
{
    std::vector<nlohmann::ordered_json> wires;
    wires.reserve(orders.size());
    for (const auto& req : orders)
        wires.push_back(order_request_to_wire(req, asset_index(req.coin)));

    auto action = order_wires_to_action(std::move(wires), grouping, std::move(builder));
    return post_action(std::move(action));
}

nlohmann::json Exchange::market_open(
    std::string_view coin, bool is_buy, double sz,
    double slippage, std::optional<Cloid> cloid)
{
    double px = slippage_price(coin, is_buy, slippage);
    return order(coin, is_buy, sz, px,
                 LimitOrderType{Tif::Ioc}, false, std::move(cloid));
}

nlohmann::json Exchange::market_close(
    std::string_view coin, std::optional<double> sz,
    double slippage, std::optional<Cloid> cloid)
{
    // Determine position size and direction from user state
    auto state = info_->user_state(wallet_);
    for (const auto& pos_entry : state["assetPositions"]) {
        const auto& pos = pos_entry["position"];
        if (pos.value("coin", "") == coin) {
            double szi  = std::stod(pos["szi"].get<std::string>());
            bool is_buy = szi < 0.0; // close a short by buying
            double close_sz = sz ? *sz : std::abs(szi);
            double px = slippage_price(coin, is_buy, slippage);
            return order(coin, is_buy, close_sz, px,
                         LimitOrderType{Tif::Ioc}, true, std::move(cloid));
        }
    }
    throw std::runtime_error(std::string("No open position for ") + std::string(coin));
}

// ── Cancel ────────────────────────────────────────────────────────────────────

nlohmann::json Exchange::cancel(std::string_view coin, int64_t oid) {
    return bulk_cancel({{std::string(coin), oid}});
}

nlohmann::json Exchange::cancel_by_cloid(std::string_view coin, const Cloid& cloid) {
    return bulk_cancel_by_cloid({{std::string(coin), cloid}});
}

nlohmann::json Exchange::bulk_cancel(std::vector<CancelRequest> cancels) {
    nlohmann::ordered_json action;
    action["type"] = "cancel";
    auto arr = nlohmann::ordered_json::array();
    for (const auto& c : cancels) {
        nlohmann::ordered_json item;
        item["a"] = asset_index(c.coin);
        item["o"] = c.oid;
        arr.push_back(std::move(item));
    }
    action["cancels"] = std::move(arr);
    return post_action(std::move(action));
}

nlohmann::json Exchange::bulk_cancel_by_cloid(std::vector<CancelByCloidRequest> cancels) {
    nlohmann::ordered_json action;
    action["type"] = "cancelByCloid";
    auto arr = nlohmann::ordered_json::array();
    for (const auto& c : cancels) {
        nlohmann::ordered_json item;
        item["asset"] = asset_index(c.coin);
        item["cloid"] = std::string(c.cloid.to_raw());
        arr.push_back(std::move(item));
    }
    action["cancels"] = std::move(arr);
    return post_action(std::move(action));
}

nlohmann::json Exchange::schedule_cancel(std::optional<int64_t> time_ms) {
    nlohmann::ordered_json action;
    action["type"] = "scheduleCancel";
    if (time_ms) action["time"] = *time_ms;
    return post_action(std::move(action));
}

// ── Modify ────────────────────────────────────────────────────────────────────

nlohmann::json Exchange::modify_order(int64_t oid, const OrderRequest& new_order) {
    return bulk_modify_orders({{oid, new_order}});
}

nlohmann::json Exchange::bulk_modify_orders(std::vector<ModifyRequest> mods) {
    nlohmann::ordered_json action;
    action["type"] = "batchModify";
    auto arr = nlohmann::ordered_json::array();
    for (const auto& m : mods) {
        nlohmann::ordered_json item;
        item["oid"]   = m.oid;
        item["order"] = order_request_to_wire(m.order, asset_index(m.order.coin));
        arr.push_back(std::move(item));
    }
    action["modifies"] = std::move(arr);
    return post_action(std::move(action));
}

// ── Leverage & margin ─────────────────────────────────────────────────────────

nlohmann::json Exchange::update_leverage(
    std::string_view coin, bool is_cross, int leverage)
{
    nlohmann::ordered_json action;
    action["type"]     = "updateLeverage";
    action["asset"]    = asset_index(coin);
    action["isCross"]  = is_cross;
    action["leverage"] = leverage;
    return post_action(std::move(action));
}

nlohmann::json Exchange::update_isolated_margin(
    std::string_view coin, bool is_buy, double ntl)
{
    nlohmann::ordered_json action;
    action["type"]  = "updateIsolatedMargin";
    action["asset"] = asset_index(coin);
    action["isBuy"] = is_buy;
    action["ntl"]   = float_to_usd_int(ntl);
    return post_action(std::move(action));
}

// ── Transfers ─────────────────────────────────────────────────────────────────

nlohmann::json Exchange::usd_class_transfer(double amount, bool to_perp) {
    nlohmann::ordered_json action;
    action["type"]    = "usdClassTransfer";
    action["amount"]  = float_to_wire(amount);
    action["toPerp"]  = to_perp;

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"amount",           "string"},
        {"toPerp",           "bool"},
        {"nonce",            "uint64"},
    };
    return post_user_signed_action(std::move(action), types,
                                    "HyperliquidTransaction:UsdClassTransfer");
}

nlohmann::json Exchange::usd_transfer(double amount, std::string_view destination) {
    nlohmann::ordered_json action;
    action["type"]        = "usdSend";
    action["destination"] = destination;
    action["amount"]      = float_to_wire(amount);

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"destination",      "string"},
        {"amount",           "string"},
        {"time",             "uint64"},
    };
    return post_user_signed_action(std::move(action), types,
                                    "HyperliquidTransaction:UsdSend");
}

nlohmann::json Exchange::spot_transfer(
    double amount, std::string_view destination, std::string_view token)
{
    nlohmann::ordered_json action;
    action["type"]        = "spotSend";
    action["destination"] = destination;
    action["token"]       = token;
    action["amount"]      = float_to_wire(amount);

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"destination",      "string"},
        {"token",            "string"},
        {"amount",           "string"},
        {"time",             "uint64"},
    };
    return post_user_signed_action(std::move(action), types,
                                    "HyperliquidTransaction:SpotSend");
}

nlohmann::json Exchange::withdraw_from_bridge(
    double amount, std::string_view destination)
{
    nlohmann::ordered_json action;
    action["type"]        = "withdraw3";
    action["destination"] = destination;
    action["amount"]      = float_to_wire(amount);

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"destination",      "string"},
        {"amount",           "string"},
        {"time",             "uint64"},
    };
    return post_user_signed_action(std::move(action), types,
                                    "HyperliquidTransaction:Withdraw");
}

// ── Agent / builder ───────────────────────────────────────────────────────────

nlohmann::json Exchange::approve_agent(
    std::string_view agent_address, std::string_view agent_name)
{
    nlohmann::ordered_json action;
    action["type"]         = "approveAgent";
    action["agentAddress"] = agent_address;
    action["agentName"]    = agent_name;

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"agentAddress",     "address"},
        {"agentName",        "string"},
        {"nonce",            "uint64"},
    };
    return post_user_signed_action(std::move(action), types,
                                    "HyperliquidTransaction:ApproveAgent");
}

nlohmann::json Exchange::approve_builder_fee(
    std::string_view builder, std::string_view max_fee_rate)
{
    nlohmann::ordered_json action;
    action["type"]       = "approveBuilderFee";
    action["maxFeeRate"] = max_fee_rate;
    action["builder"]    = builder;

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"maxFeeRate",       "string"},
        {"builder",          "address"},
        {"nonce",            "uint64"},
    };
    return post_user_signed_action(std::move(action), types,
                                    "HyperliquidTransaction:ApproveBuilderFee");
}

// ── Sub-accounts ──────────────────────────────────────────────────────────────

nlohmann::json Exchange::create_sub_account(std::string_view name) {
    nlohmann::ordered_json action;
    action["type"] = "createSubAccount";
    action["name"] = name;
    return post_action(std::move(action));
}

nlohmann::json Exchange::sub_account_transfer(
    double usd, bool to_sub, std::string_view sub_account_user)
{
    nlohmann::ordered_json action;
    action["type"]           = "subAccountTransfer";
    action["subAccountUser"] = sub_account_user;
    action["isDeposit"]      = to_sub;
    action["usd"]            = float_to_usd_int(usd);
    return post_action(std::move(action));
}

} // namespace hyperliquid
