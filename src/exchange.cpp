#include <hyperliquid/exchange.hpp>
#include <hyperliquid/utils/constants.hpp>
#include <hyperliquid/utils/signing.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string transfer_amount_to_string(double amount) {
    std::ostringstream oss;
    oss << std::setprecision(15) << std::defaultfloat << amount;
    std::string value = oss.str();
    if (value.find_first_of(".eE") == std::string::npos)
        value += ".0";
    return value;
}

double round_to_significant(double value, int significant_figures) {
    std::ostringstream oss;
    oss << std::setprecision(significant_figures) << std::defaultfloat << value;
    return std::stod(oss.str());
}

} // namespace

namespace hyperliquid {

using signing::float_to_usd_int;
using signing::get_timestamp_ms;
using signing::order_request_to_wire;
using signing::order_wires_to_action;
using signing::private_key_to_address;
using signing::sign_l1_action;
using signing::sign_user_signed_action;

Exchange::Exchange(
    std::string private_key_hex,
    std::string_view base_url,
    std::shared_ptr<Info> info,
    std::optional<std::string> vault_address)
    : Exchange(std::move(private_key_hex), base_url, std::move(info), std::move(vault_address), {})
{
}

Exchange::Exchange(
    std::string private_key_hex,
    std::string_view base_url,
    std::shared_ptr<Info> info,
    std::optional<std::string> vault_address,
    std::optional<std::string> account_address)
    : Api(base_url)
    , private_key_(std::move(private_key_hex))
    , vault_address_(std::move(vault_address))
    , account_address_(std::move(account_address))
    , info_(std::move(info))
    , is_mainnet_(base_url == MAINNET_API_URL)
{
    wallet_ = private_key_to_address(private_key_);
    info_->load_meta();
}

// Helpers

int Exchange::asset_index(std::string_view coin) {
    return info_->name_to_asset(coin);
}

std::string Exchange::effective_account_address() const {
    if (vault_address_)
        return *vault_address_;
    if (account_address_)
        return *account_address_;
    return wallet_;
}

nlohmann::json Exchange::post_action(nlohmann::ordered_json action) {
    return post_action(std::move(action), vault_address_, vault_address_);
}

nlohmann::json Exchange::post_action(
    nlohmann::ordered_json action,
    std::optional<std::string> signing_vault_address,
    std::optional<std::string> payload_vault_address)
{
    const int64_t nonce = get_timestamp_ms();
    const auto sig = sign_l1_action(
        private_key_, action, signing_vault_address, nonce, is_mainnet_);

    nlohmann::json req;
    req["action"] = action;
    req["nonce"] = nonce;
    req["signature"] = {{"r", sig.r}, {"s", sig.s}, {"v", sig.v}};
    if (payload_vault_address)
        req["vaultAddress"] = *payload_vault_address;
    else
        req["vaultAddress"] = nullptr;

    return post("/exchange", req);
}

nlohmann::json Exchange::post_user_signed_action(
    nlohmann::ordered_json action,
    const std::vector<std::pair<std::string, std::string>>& payload_types,
    std::string_view primary_type)
{
    return post_user_signed_action(std::move(action), payload_types, primary_type, vault_address_);
}

nlohmann::json Exchange::post_user_signed_action(
    nlohmann::ordered_json action,
    const std::vector<std::pair<std::string, std::string>>& payload_types,
    std::string_view primary_type,
    std::optional<std::string> payload_vault_address)
{
    const int64_t nonce = get_timestamp_ms();

    const char* ts_key = "nonce";
    for (const auto& [field_name, _field_type] : payload_types) {
        if (field_name == "time") {
            ts_key = "time";
            break;
        }
    }
    action[ts_key] = nonce;

    const auto sig = sign_user_signed_action(
        private_key_, action, payload_types, primary_type, is_mainnet_);

    nlohmann::json req;
    req["action"] = action;
    req["nonce"] = nonce;
    req["signature"] = {{"r", sig.r}, {"s", sig.s}, {"v", sig.v}};
    if (payload_vault_address)
        req["vaultAddress"] = *payload_vault_address;
    else
        req["vaultAddress"] = nullptr;

    return post("/exchange", req);
}

double Exchange::slippage_price(
    std::string_view coin, bool is_buy, double slippage, std::optional<double> px)
{
    const std::string canonical_coin = info_->canonical_coin(coin);
    const int asset = asset_index(coin);
    const bool is_spot = asset >= 10000;
    const int max_decimals = is_spot ? 8 : 6;
    const int decimal_places =
        std::max(0, max_decimals - info_->asset_to_sz_decimals.at(asset));

    double price = px.value_or(std::stod(info_->all_mids().at(canonical_coin).get<std::string>()));
    price *= is_buy ? (1.0 + slippage) : (1.0 - slippage);
    price = round_to_significant(price, 5);

    const double scale = std::pow(10.0, decimal_places);
    return std::round(price * scale) / scale;
}

// Order management

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
    req.coin = std::string(coin);
    req.is_buy = is_buy;
    req.sz = sz;
    req.limit_px = limit_px;
    req.order_type = order_type;
    req.reduce_only = reduce_only;
    req.cloid = std::move(cloid);

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

    if (builder)
        builder->b = lowercase_copy(builder->b);

    auto action = order_wires_to_action(std::move(wires), grouping, std::move(builder));
    return post_action(std::move(action));
}

nlohmann::json Exchange::market_open(
    std::string_view coin, bool is_buy, double sz,
    double slippage, std::optional<Cloid> cloid)
{
    const double px = slippage_price(coin, is_buy, slippage);
    return order(coin, is_buy, sz, px,
                 LimitOrderType{Tif::Ioc}, false, std::move(cloid));
}

nlohmann::json Exchange::market_close(
    std::string_view coin, std::optional<double> sz,
    double slippage, std::optional<Cloid> cloid)
{
    const std::string canonical_coin = info_->canonical_coin(coin);
    const auto state = info_->user_state(effective_account_address());
    for (const auto& pos_entry : state.at("assetPositions")) {
        const auto& pos = pos_entry.at("position");
        if (pos.value("coin", "") != canonical_coin)
            continue;

        const double szi = std::stod(pos.at("szi").get<std::string>());
        const bool is_buy = szi < 0.0;
        const double close_sz = sz.value_or(std::abs(szi));
        const double px = slippage_price(canonical_coin, is_buy, slippage);
        return order(canonical_coin, is_buy, close_sz, px,
                     LimitOrderType{Tif::Ioc}, true, std::move(cloid));
    }

    throw std::runtime_error("No open position for " + std::string(coin));
}

// Cancel

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
    for (const auto& cancel : cancels) {
        nlohmann::ordered_json item;
        item["a"] = asset_index(cancel.coin);
        item["o"] = cancel.oid;
        arr.push_back(std::move(item));
    }
    action["cancels"] = std::move(arr);
    return post_action(std::move(action));
}

nlohmann::json Exchange::bulk_cancel_by_cloid(std::vector<CancelByCloidRequest> cancels) {
    nlohmann::ordered_json action;
    action["type"] = "cancelByCloid";
    auto arr = nlohmann::ordered_json::array();
    for (const auto& cancel : cancels) {
        nlohmann::ordered_json item;
        item["asset"] = asset_index(cancel.coin);
        item["cloid"] = std::string(cancel.cloid.to_raw());
        arr.push_back(std::move(item));
    }
    action["cancels"] = std::move(arr);
    return post_action(std::move(action));
}

nlohmann::json Exchange::schedule_cancel(std::optional<int64_t> time_ms) {
    nlohmann::ordered_json action;
    action["type"] = "scheduleCancel";
    if (time_ms)
        action["time"] = *time_ms;
    return post_action(std::move(action));
}

// Modify

nlohmann::json Exchange::modify_order(int64_t oid, const OrderRequest& new_order) {
    return bulk_modify_orders({{oid, new_order}});
}

nlohmann::json Exchange::bulk_modify_orders(std::vector<ModifyRequest> mods) {
    nlohmann::ordered_json action;
    action["type"] = "batchModify";
    auto arr = nlohmann::ordered_json::array();
    for (const auto& mod : mods) {
        nlohmann::ordered_json item;
        item["oid"] = mod.oid;
        item["order"] = order_request_to_wire(mod.order, asset_index(mod.order.coin));
        arr.push_back(std::move(item));
    }
    action["modifies"] = std::move(arr);
    return post_action(std::move(action));
}

// Leverage & margin

nlohmann::json Exchange::update_leverage(
    std::string_view coin, bool is_cross, int leverage)
{
    nlohmann::ordered_json action;
    action["type"] = "updateLeverage";
    action["asset"] = asset_index(coin);
    action["isCross"] = is_cross;
    action["leverage"] = leverage;
    return post_action(std::move(action));
}

nlohmann::json Exchange::update_isolated_margin(std::string_view coin, double amount) {
    nlohmann::ordered_json action;
    action["type"] = "updateIsolatedMargin";
    action["asset"] = asset_index(coin);
    action["isBuy"] = true;
    action["ntli"] = float_to_usd_int(amount);
    return post_action(std::move(action));
}

// Transfers

nlohmann::json Exchange::usd_class_transfer(double amount, bool to_perp) {
    nlohmann::ordered_json action;
    action["type"] = "usdClassTransfer";
    action["amount"] = transfer_amount_to_string(amount);
    if (vault_address_)
        action["amount"] = action["amount"].get<std::string>() + " subaccount:" + *vault_address_;
    action["toPerp"] = to_perp;

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"amount", "string"},
        {"toPerp", "bool"},
        {"nonce", "uint64"},
    };
    return post_user_signed_action(
        std::move(action), types, "HyperliquidTransaction:UsdClassTransfer", {});
}

nlohmann::json Exchange::usd_transfer(double amount, std::string_view destination) {
    nlohmann::ordered_json action;
    action["type"] = "usdSend";
    action["destination"] = destination;
    action["amount"] = transfer_amount_to_string(amount);

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"destination", "string"},
        {"amount", "string"},
        {"time", "uint64"},
    };
    return post_user_signed_action(std::move(action), types, "HyperliquidTransaction:UsdSend");
}

nlohmann::json Exchange::spot_transfer(
    double amount, std::string_view destination, std::string_view token)
{
    nlohmann::ordered_json action;
    action["type"] = "spotSend";
    action["destination"] = destination;
    action["token"] = token;
    action["amount"] = transfer_amount_to_string(amount);

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"destination", "string"},
        {"token", "string"},
        {"amount", "string"},
        {"time", "uint64"},
    };
    return post_user_signed_action(std::move(action), types, "HyperliquidTransaction:SpotSend");
}

nlohmann::json Exchange::withdraw_from_bridge(
    double amount, std::string_view destination)
{
    nlohmann::ordered_json action;
    action["type"] = "withdraw3";
    action["destination"] = destination;
    action["amount"] = transfer_amount_to_string(amount);

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"destination", "string"},
        {"amount", "string"},
        {"time", "uint64"},
    };
    return post_user_signed_action(std::move(action), types, "HyperliquidTransaction:Withdraw");
}

// Agent / builder

nlohmann::json Exchange::approve_agent(
    std::string_view agent_address, std::string_view agent_name)
{
    nlohmann::ordered_json action;
    action["type"] = "approveAgent";
    action["agentAddress"] = lowercase_copy(std::string(agent_address));
    action["agentName"] = agent_name;

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"agentAddress", "address"},
        {"agentName", "string"},
        {"nonce", "uint64"},
    };
    return post_user_signed_action(std::move(action), types, "HyperliquidTransaction:ApproveAgent");
}

nlohmann::json Exchange::approve_builder_fee(
    std::string_view builder, std::string_view max_fee_rate)
{
    nlohmann::ordered_json action;
    action["type"] = "approveBuilderFee";
    action["maxFeeRate"] = max_fee_rate;
    action["builder"] = lowercase_copy(std::string(builder));

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"maxFeeRate", "string"},
        {"builder", "address"},
        {"nonce", "uint64"},
    };
    return post_user_signed_action(
        std::move(action), types, "HyperliquidTransaction:ApproveBuilderFee");
}

// Sub-accounts

nlohmann::json Exchange::create_sub_account(std::string_view name) {
    nlohmann::ordered_json action;
    action["type"] = "createSubAccount";
    action["name"] = name;
    return post_action(std::move(action), {}, {});
}

nlohmann::json Exchange::sub_account_transfer(
    double usd, bool to_sub, std::string_view sub_account_user)
{
    nlohmann::ordered_json action;
    action["type"] = "subAccountTransfer";
    action["subAccountUser"] = lowercase_copy(std::string(sub_account_user));
    action["isDeposit"] = to_sub;
    action["usd"] = float_to_usd_int(usd);
    return post_action(std::move(action), {}, {});
}

} // namespace hyperliquid
