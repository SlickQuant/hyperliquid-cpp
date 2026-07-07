#pragma once

#include <hyperliquid/api.hpp>
#include <hyperliquid/info.hpp>
#include <hyperliquid/utils/types.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace hyperliquid {

// Authenticated Hyperliquid trading client.
// Mirrors the Python SDK's Exchange class.
//
// private_key_hex: 32-byte secp256k1 private key as hex ("0x..." or bare hex).
// base_url: MAINNET_API_URL or TESTNET_API_URL.
// info: shared Info instance for asset-index lookups and market data.
// vault_address: optional sub-account / vault address; signs on behalf of it.
// account_address: optional sub-account address; signs on behalf of it.
//
// All trading methods throw Api::HttpError on HTTP 4xx/5xx, std::runtime_error
// on network or signing failure, and std::invalid_argument on non-finite or
// unrepresentable price/size values (propagated from float_to_wire).
class Exchange : public Api {
public:
    // Throws std::runtime_error if private_key_hex is not a valid secp256k1 key.
    Exchange(std::string private_key_hex,
             std::string_view base_url,
             std::shared_ptr<Info> info,
             std::optional<std::string> vault_address = {});
    // Throws std::runtime_error if private_key_hex is not a valid secp256k1 key.
    Exchange(std::string private_key_hex,
             std::string_view base_url,
             std::shared_ptr<Info> info,
             std::optional<std::string> vault_address,
             std::optional<std::string> account_address);

    // Ethereum address derived from private_key_hex.
    const std::string& wallet_address() const noexcept { return wallet_; }

    // ── Order management ──────────────────────────────────────────────────────

    nlohmann::json order(std::string_view coin, bool is_buy,
                         double sz, double limit_px,
                         const OrderType& order_type,
                         bool reduce_only = false,
                         std::optional<Cloid> cloid = {},
                         std::optional<BuilderInfo> builder = {},
                         const std::string& grouping = "na");

    nlohmann::json bulk_orders(std::vector<OrderRequest> orders,
                                std::optional<BuilderInfo> builder = {},
                                const std::string& grouping = "na");

    // Market order: IoC limit at mid-price +/- slippage (default 5%).
    nlohmann::json market_open(std::string_view coin, bool is_buy, double sz,
                                double slippage = 0.05,
                                std::optional<Cloid> cloid = {});

    // Close current position at mid-price +/- slippage. Queries user_state for sz.
    // Throws std::runtime_error if there is no open position for coin.
    nlohmann::json market_close(std::string_view coin,
                                 std::optional<double> sz = {},
                                 double slippage = 0.05,
                                 std::optional<Cloid> cloid = {});

    // ── Cancel ────────────────────────────────────────────────────────────────

    nlohmann::json cancel(std::string_view coin, int64_t oid);
    nlohmann::json cancel_by_cloid(std::string_view coin, const Cloid& cloid);
    nlohmann::json bulk_cancel(std::vector<CancelRequest> cancels);
    nlohmann::json bulk_cancel_by_cloid(std::vector<CancelByCloidRequest> cancels);

    // Schedule a future mass cancel. Pass {} for no time (cancel immediately on trigger).
    nlohmann::json schedule_cancel(std::optional<int64_t> time_ms = {});

    // ── Modify ────────────────────────────────────────────────────────────────

    nlohmann::json modify_order(int64_t oid, const OrderRequest& new_order);
    nlohmann::json bulk_modify_orders(std::vector<ModifyRequest> mods);

    // ── Leverage & margin ─────────────────────────────────────────────────────

    nlohmann::json update_leverage(std::string_view coin, bool is_cross, int leverage);

    // amount: signed USD amount for isolated margin adjustment.
    nlohmann::json update_isolated_margin(std::string_view coin, double amount);

    // ── Transfers (user-signed) ───────────────────────────────────────────────

    // Transfer USD between perpetuals and spot wallets.
    nlohmann::json usd_class_transfer(double amount, bool to_perp);

    // Send USD to another address (USDC).
    nlohmann::json usd_transfer(double amount, std::string_view destination);

    // Send spot tokens to another address.
    nlohmann::json spot_transfer(double amount, std::string_view destination,
                                  std::string_view token);

    // Initiate a withdrawal from the bridge to an L1 address.
    nlohmann::json withdraw_from_bridge(double amount,
                                         std::string_view destination);

    // ── Agent / builder ───────────────────────────────────────────────────────

    nlohmann::json approve_agent(std::string_view agent_address,
                                  std::string_view agent_name = "");

    nlohmann::json approve_builder_fee(std::string_view builder,
                                        std::string_view max_fee_rate);

    // ── Sub-account management ────────────────────────────────────────────────

    nlohmann::json create_sub_account(std::string_view name);
    nlohmann::json sub_account_transfer(double usd, bool to_sub,
                                         std::string_view sub_account_user);

private:
    std::string private_key_;
    std::string wallet_;
    std::optional<std::string> vault_address_;
    std::optional<std::string> account_address_;
    std::shared_ptr<Info> info_;
    bool is_mainnet_;

    int asset_index(std::string_view coin);
    std::string effective_account_address() const;

    // Sign an L1 action and POST to /exchange.
    nlohmann::json post_action(nlohmann::ordered_json action);
    nlohmann::json post_action(nlohmann::ordered_json action,
                               std::optional<std::string> signing_vault_address,
                               std::optional<std::string> payload_vault_address);

    // Sign a user-signed action and POST to /exchange.
    nlohmann::json post_user_signed_action(
        nlohmann::ordered_json action,
        const std::vector<std::pair<std::string, std::string>>& payload_types,
        std::string_view primary_type);
    nlohmann::json post_user_signed_action(
        nlohmann::ordered_json action,
        const std::vector<std::pair<std::string, std::string>>& payload_types,
        std::string_view primary_type,
        std::optional<std::string> payload_vault_address);

    // Apply slippage to mid-price and round to asset precision.
    double slippage_price(std::string_view coin, bool is_buy, double slippage,
                          std::optional<double> px = {});
};

} // namespace hyperliquid
