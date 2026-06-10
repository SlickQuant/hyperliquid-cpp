#include <gtest/gtest.h>

#include <hyperliquid/exchange.hpp>
#include <hyperliquid/info.hpp>
#include <hyperliquid/utils/constants.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

using hyperliquid::Exchange;
using hyperliquid::Info;
using hyperliquid::LOCAL_API_URL;
using hyperliquid::TESTNET_API_URL;
using json = nlohmann::json;

namespace {

constexpr const char* kTestKey =
    "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
constexpr const char* kVaultAddress =
    "0x00000000000000000000000000000000000000aa";
constexpr const char* kAccountAddress =
    "0x00000000000000000000000000000000000000bb";
constexpr const char* kDestination =
    "0x00000000000000000000000000000000000000cc";

json make_meta() {
    return {
        {"universe",
         json::array({
             {{"name", "ETH"}, {"szDecimals", 4}},
             {{"name", "BTC"}, {"szDecimals", 5}},
         })},
    };
}

json make_spot_meta() {
    return {
        {"tokens",
         json::array({
             {{"index", 0}, {"name", "PURR"}, {"szDecimals", 5}},
             {{"index", 1}, {"name", "USDC"}, {"szDecimals", 6}},
         })},
        {"universe",
         json::array({
             {{"index", 0}, {"name", "@0"}, {"tokens", json::array({0, 1})}, {"isCanonical", true}},
         })},
    };
}

class MockInfo : public Info {
public:
    MockInfo()
        : Info(LOCAL_API_URL, /*skip_ws=*/true) {}

    json meta_response = make_meta();
    json spot_meta_response = make_spot_meta();
    json all_mids_response = {
        {"ETH", "2000.0"},
        {"BTC", "60000.0"},
        {"@0", "0.12345678"},
    };
    json user_state_response = {
        {"assetPositions",
         json::array({
             {{"position", {{"coin", "ETH"}, {"szi", "1.5"}}}},
         })},
    };

    std::string last_endpoint;
    json last_payload;
    std::vector<json> requests;

    void clear_requests() {
        requests.clear();
        last_endpoint.clear();
        last_payload = json();
    }

    json post(std::string_view endpoint, const json& payload) override {
        last_endpoint = std::string(endpoint);
        last_payload = payload;
        requests.push_back(payload);

        if (endpoint != "/info")
            return json::object();

        const std::string type = payload.at("type").get<std::string>();
        if (type == "meta")
            return meta_response;
        if (type == "spotMeta")
            return spot_meta_response;
        if (type == "allMids")
            return all_mids_response;
        if (type == "clearinghouseState")
            return user_state_response;
        if (type == "l2Book") {
            return {
                {"coin", payload.at("coin")},
                {"levels", json::array({json::array(), json::array()})},
            };
        }
        return json::object();
    }
};

class MockExchange : public Exchange {
public:
    using Exchange::Exchange;

    std::string last_endpoint;
    json last_payload;

    json post(std::string_view endpoint, const json& payload) override {
        last_endpoint = std::string(endpoint);
        last_payload = payload;
        return {
            {"status", "ok"},
        };
    }
};

} // namespace

TEST(InfoMappings, NameToAssetResolvesPerpAndSpotMarkets) {
    MockInfo info;
    info.load_meta();

    EXPECT_EQ(info.name_to_asset("ETH"), 0);
    EXPECT_EQ(info.name_to_asset("@0"), 10000);
    EXPECT_EQ(info.name_to_asset("PURR/USDC"), 10000);
    EXPECT_EQ(info.canonical_coin("PURR/USDC"), "@0");
    EXPECT_EQ(info.asset_to_sz_decimals.at(10000), 5);
}

TEST(InfoMappings, L2SnapshotRemapsSpotPairToCanonicalCoin) {
    MockInfo info;
    auto book = info.l2_snapshot("PURR/USDC");

    EXPECT_EQ(info.last_payload["type"].get<std::string>(), "l2Book");
    EXPECT_EQ(info.last_payload["coin"].get<std::string>(), "@0");
    EXPECT_EQ(book["coin"].get<std::string>(), "@0");
}

TEST(ExchangePayloads, UpdateIsolatedMarginUsesNtliWireField) {
    auto info = std::make_shared<MockInfo>();
    MockExchange exchange(kTestKey, TESTNET_API_URL, info);

    exchange.update_isolated_margin("ETH", -12.345678);

    ASSERT_EQ(exchange.last_endpoint, "/exchange");
    const auto& action = exchange.last_payload["action"];
    EXPECT_EQ(action["type"].get<std::string>(), "updateIsolatedMargin");
    EXPECT_EQ(action["asset"].get<int>(), 0);
    EXPECT_TRUE(action["isBuy"].get<bool>());
    EXPECT_EQ(action["ntli"].get<int64_t>(), -12345678LL);
    EXPECT_FALSE(action.contains("ntl"));
}

TEST(ExchangePayloads, UsdClassTransferAppendsSubaccountSuffix) {
    auto info = std::make_shared<MockInfo>();
    MockExchange exchange(
        kTestKey, TESTNET_API_URL, info, std::optional<std::string>{kVaultAddress});

    exchange.usd_class_transfer(1.0, true);

    ASSERT_EQ(exchange.last_endpoint, "/exchange");
    const auto& action = exchange.last_payload["action"];
    EXPECT_EQ(action["amount"].get<std::string>(),
              std::string("1.0 subaccount:") + kVaultAddress);
    EXPECT_TRUE(exchange.last_payload["vaultAddress"].is_null());
}

TEST(ExchangePayloads, UserSignedTransfersIncludeVaultAddress) {
    auto info = std::make_shared<MockInfo>();
    MockExchange exchange(
        kTestKey, TESTNET_API_URL, info, std::optional<std::string>{kVaultAddress});

    exchange.usd_transfer(1.0, kDestination);

    ASSERT_EQ(exchange.last_endpoint, "/exchange");
    EXPECT_EQ(exchange.last_payload["vaultAddress"].get<std::string>(), kVaultAddress);
    EXPECT_EQ(exchange.last_payload["action"]["amount"].get<std::string>(), "1.0");
}

TEST(ExchangePricing, MarketOpenRoundsSpotPriceUsingSdkRules) {
    auto info = std::make_shared<MockInfo>();
    MockExchange exchange(kTestKey, TESTNET_API_URL, info);

    exchange.market_open("PURR/USDC", true, 1.0);

    const auto& order = exchange.last_payload["action"]["orders"][0];
    EXPECT_EQ(order["a"].get<int>(), 10000);
    EXPECT_EQ(order["p"].get<std::string>(), "0.13");
}

TEST(ExchangeAccountSelection, MarketCloseUsesExplicitAccountAddress) {
    auto info = std::make_shared<MockInfo>();
    MockExchange exchange(
        kTestKey,
        TESTNET_API_URL,
        info,
        std::optional<std::string>{},
        std::optional<std::string>{kAccountAddress});

    info->clear_requests();
    exchange.market_close("ETH");

    ASSERT_GE(info->requests.size(), 2u);
    EXPECT_EQ(info->requests[0]["type"].get<std::string>(), "clearinghouseState");
    EXPECT_EQ(info->requests[0]["user"].get<std::string>(), kAccountAddress);
}

TEST(ExchangeAccountSelection, MarketClosePrefersVaultAddressOverAccountAddress) {
    auto info = std::make_shared<MockInfo>();
    MockExchange exchange(
        kTestKey,
        TESTNET_API_URL,
        info,
        std::optional<std::string>{kVaultAddress},
        std::optional<std::string>{kAccountAddress});

    info->clear_requests();
    exchange.market_close("ETH");

    ASSERT_GE(info->requests.size(), 2u);
    EXPECT_EQ(info->requests[0]["type"].get<std::string>(), "clearinghouseState");
    EXPECT_EQ(info->requests[0]["user"].get<std::string>(), kVaultAddress);
}
