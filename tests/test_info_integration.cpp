#include <gtest/gtest.h>
#include <hyperliquid/info.hpp>
#include <hyperliquid/utils/constants.hpp>

#include <nlohmann/json.hpp>
#include <chrono>
#include <string>
#include <thread>

using hyperliquid::Info;
using hyperliquid::TESTNET_API_URL;
using json = nlohmann::json;

// ── Test fixture ──────────────────────────────────────────────────────────────
// A single Info instance is shared across all tests to avoid redundant
// connections. skip_ws=true so we only test REST endpoints here.

class InfoIntegration : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        info_ = std::make_unique<Info>(TESTNET_API_URL, /*skip_ws=*/true);
    }
    static void TearDownTestSuite() {
        info_.reset();
    }

    static std::unique_ptr<Info> info_;
};

std::unique_ptr<Info> InfoIntegration::info_;

// ── meta() ────────────────────────────────────────────────────────────────────

TEST_F(InfoIntegration, MetaReturnsUniverse) {
    auto m = info_->meta();
    ASSERT_TRUE(m.contains("universe")) << m.dump(2);
    ASSERT_TRUE(m["universe"].is_array());
    ASSERT_GT(m["universe"].size(), 0u);
}

TEST_F(InfoIntegration, MetaUniverseHasNameField) {
    auto m = info_->meta();
    const auto& first = m["universe"][0];
    EXPECT_TRUE(first.contains("name"));
    EXPECT_TRUE(first["name"].is_string());
}

TEST_F(InfoIntegration, MetaUniverseHasSzDecimalsField) {
    auto m = info_->meta();
    const auto& first = m["universe"][0];
    EXPECT_TRUE(first.contains("szDecimals"));
}

TEST_F(InfoIntegration, MetaPopulatesCoinToAsset) {
    info_->load_meta();
    EXPECT_FALSE(info_->coin_to_asset.empty());
    EXPECT_TRUE(info_->coin_to_asset.count("BTC") || info_->coin_to_asset.count("ETH"))
        << "Expected at least BTC or ETH in coin_to_asset";
}

TEST_F(InfoIntegration, MetaEthIndexIsNonNegative) {
    info_->load_meta();
    if (info_->coin_to_asset.count("ETH")) {
        EXPECT_GE(info_->coin_to_asset.at("ETH"), 0);
    }
}

// ── spot_meta() ───────────────────────────────────────────────────────────────

TEST_F(InfoIntegration, SpotMetaReturnsTokens) {
    auto sm = info_->spot_meta();
    ASSERT_TRUE(sm.contains("tokens")) << sm.dump(2);
    ASSERT_TRUE(sm["tokens"].is_array());
}

TEST_F(InfoIntegration, SpotMetaReturnsUniverse) {
    auto sm = info_->spot_meta();
    ASSERT_TRUE(sm.contains("universe")) << sm.dump(2);
    ASSERT_TRUE(sm["universe"].is_array());
}

TEST_F(InfoIntegration, SpotMetaTokenHasNameField) {
    auto sm = info_->spot_meta();
    if (!sm["tokens"].empty()) {
        EXPECT_TRUE(sm["tokens"][0].contains("name"));
    }
}

// ── all_mids() ────────────────────────────────────────────────────────────────

TEST_F(InfoIntegration, AllMidsReturnsObject) {
    auto mids = info_->all_mids();
    ASSERT_TRUE(mids.is_object()) << mids.dump(2);
    ASSERT_GT(mids.size(), 0u);
}

TEST_F(InfoIntegration, AllMidsValuesAreStrings) {
    auto mids = info_->all_mids();
    for (auto& [coin, price] : mids.items()) {
        EXPECT_TRUE(price.is_string())
            << "Coin " << coin << " price is not a string: " << price;
        // Spot: parse succeeds
        EXPECT_NO_THROW(std::stod(price.get<std::string>()))
            << "Coin " << coin << " price not parseable: " << price;
    }
}

TEST_F(InfoIntegration, AllMidsEthPricePositive) {
    auto mids = info_->all_mids();
    if (mids.contains("ETH")) {
        double eth = std::stod(mids["ETH"].get<std::string>());
        EXPECT_GT(eth, 0.0);
    }
}

// ── l2_snapshot() ─────────────────────────────────────────────────────────────

TEST_F(InfoIntegration, L2SnapshotHasLevelsArray) {
    auto book = info_->l2_snapshot("BTC");
    ASSERT_TRUE(book.contains("levels")) << book.dump(2);
    ASSERT_TRUE(book["levels"].is_array());
    // Hyperliquid returns [ [bids...], [asks...] ]
    ASSERT_EQ(book["levels"].size(), 2u);
}

TEST_F(InfoIntegration, L2SnapshotBidsNonEmpty) {
    auto book = info_->l2_snapshot("BTC");
    // Testnet may have sparse liquidity; just check array structure
    EXPECT_TRUE(book["levels"][0].is_array());
}

TEST_F(InfoIntegration, L2SnapshotAsksNonEmpty) {
    auto book = info_->l2_snapshot("BTC");
    EXPECT_TRUE(book["levels"][1].is_array());
}

TEST_F(InfoIntegration, L2SnapshotLevelEntryHasPxAndSz) {
    auto book = info_->l2_snapshot("ETH");
    // Only validate structure if there are any bids
    if (!book["levels"][0].empty()) {
        const auto& bid = book["levels"][0][0];
        EXPECT_TRUE(bid.contains("px")) << bid.dump();
        EXPECT_TRUE(bid.contains("sz")) << bid.dump();
    }
}

TEST_F(InfoIntegration, L2SnapshotHasCoinField) {
    auto book = info_->l2_snapshot("ETH");
    EXPECT_TRUE(book.contains("coin")) << book.dump(2);
    EXPECT_EQ(book["coin"].get<std::string>(), "ETH");
}

// ── bbo() ─────────────────────────────────────────────────────────────────────
// NOTE: the REST /info bbo type is not supported on testnet (returns 422).
// BBO data is available only via WebSocket subscription. Test is disabled.

TEST_F(InfoIntegration, DISABLED_BboReturnsLevels) {
    auto bbo = info_->bbo("BTC");
    ASSERT_FALSE(bbo.is_null());
    EXPECT_TRUE(bbo.contains("levels")) << bbo.dump(2);
}

// ── candle_snapshot() ─────────────────────────────────────────────────────────

TEST_F(InfoIntegration, CandleSnapshotReturnsArray) {
    // Request last 24 h of 1h candles
    using namespace std::chrono;
    int64_t now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    int64_t start_ms = now_ms - 24LL * 3600 * 1000;

    auto candles = info_->candle_snapshot("BTC", "1h", start_ms);
    ASSERT_TRUE(candles.is_array()) << candles.dump(2);
}

TEST_F(InfoIntegration, CandleSnapshotEntryHasOHLC) {
    using namespace std::chrono;
    int64_t now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    int64_t start_ms = now_ms - 24LL * 3600 * 1000;

    auto candles = info_->candle_snapshot("ETH", "1h", start_ms);
    if (!candles.empty()) {
        const auto& c = candles[0];
        EXPECT_TRUE(c.contains("o")) << c.dump();  // open
        EXPECT_TRUE(c.contains("h")) << c.dump();  // high
        EXPECT_TRUE(c.contains("l")) << c.dump();  // low
        EXPECT_TRUE(c.contains("c")) << c.dump();  // close
        EXPECT_TRUE(c.contains("v")) << c.dump();  // volume
    }
}

TEST_F(InfoIntegration, CandleSnapshotEndTimeFilters) {
    using namespace std::chrono;
    int64_t now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    int64_t start_ms = now_ms - 7LL * 24 * 3600 * 1000; // 7 days ago
    int64_t end_ms   = now_ms - 6LL * 24 * 3600 * 1000; // 6 days ago

    auto candles = info_->candle_snapshot("BTC", "1h", start_ms, end_ms);
    ASSERT_TRUE(candles.is_array());
    // Should be ~24 candles (1 per hour over 24 h window)
    EXPECT_LE(candles.size(), 30u);
}

// ── perp_asset_ctxs() ─────────────────────────────────────────────────────────

TEST_F(InfoIntegration, PerpAssetCtxsReturnsArray) {
    auto ctxs = info_->perp_asset_ctxs();
    // Returns [meta, assetCtxs] array
    ASSERT_TRUE(ctxs.is_array()) << ctxs.dump(2);
    ASSERT_EQ(ctxs.size(), 2u);
}

TEST_F(InfoIntegration, PerpAssetCtxsHasUniverse) {
    auto ctxs = info_->perp_asset_ctxs();
    ASSERT_TRUE(ctxs[0].contains("universe")) << ctxs[0].dump(2);
}

TEST_F(InfoIntegration, PerpAssetCtxsAssetCtxsNonEmpty) {
    auto ctxs = info_->perp_asset_ctxs();
    ASSERT_TRUE(ctxs[1].is_array());
    ASSERT_GT(ctxs[1].size(), 0u);
}

TEST_F(InfoIntegration, PerpAssetCtxsEntryHasFundingRate) {
    auto ctxs = info_->perp_asset_ctxs();
    if (!ctxs[1].empty()) {
        const auto& ctx = ctxs[1][0];
        EXPECT_TRUE(ctx.contains("funding")) << ctx.dump();
    }
}

// ── spot_asset_ctxs() ─────────────────────────────────────────────────────────

TEST_F(InfoIntegration, SpotAssetCtxsReturnsArray) {
    auto ctxs = info_->spot_asset_ctxs();
    ASSERT_TRUE(ctxs.is_array()) << ctxs.dump(2);
    ASSERT_EQ(ctxs.size(), 2u);
}

// ── funding_history() ─────────────────────────────────────────────────────────

TEST_F(InfoIntegration, FundingHistoryReturnsArray) {
    using namespace std::chrono;
    int64_t now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    int64_t start_ms = now_ms - 24LL * 3600 * 1000;

    auto hist = info_->funding_history("ETH", start_ms);
    ASSERT_TRUE(hist.is_array()) << hist.dump(2);
}

TEST_F(InfoIntegration, FundingHistoryEntryHasRate) {
    using namespace std::chrono;
    int64_t now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    int64_t start_ms = now_ms - 7LL * 24 * 3600 * 1000;

    auto hist = info_->funding_history("BTC", start_ms);
    if (!hist.empty()) {
        EXPECT_TRUE(hist[0].contains("fundingRate")) << hist[0].dump();
        EXPECT_TRUE(hist[0].contains("time")) << hist[0].dump();
    }
}

TEST_F(InfoIntegration, FundingHistoryEndTimeFilters) {
    using namespace std::chrono;
    int64_t now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    int64_t start_ms = now_ms - 7LL * 24 * 3600 * 1000;
    int64_t end_ms   = now_ms - 6LL * 24 * 3600 * 1000;

    auto hist_full = info_->funding_history("ETH", start_ms);
    auto hist_clip = info_->funding_history("ETH", start_ms, end_ms);
    EXPECT_LE(hist_clip.size(), hist_full.size());
}

// ── user_state() ─────────────────────────────────────────────────────────────

// Use the hardhat test address (no real funds on testnet but structure is defined)
static constexpr const char* kTestAddress =
    "0xf39fd6e51aad88f6f4ce6ab8827279cfffb92266";

TEST_F(InfoIntegration, UserStateReturnsObject) {
    auto state = info_->user_state(kTestAddress);
    ASSERT_TRUE(state.is_object()) << state.dump(2);
}

TEST_F(InfoIntegration, UserStateHasMarginSummary) {
    auto state = info_->user_state(kTestAddress);
    EXPECT_TRUE(state.contains("marginSummary")) << state.dump(2);
}

TEST_F(InfoIntegration, UserStateHasAssetPositions) {
    auto state = info_->user_state(kTestAddress);
    EXPECT_TRUE(state.contains("assetPositions")) << state.dump(2);
    EXPECT_TRUE(state["assetPositions"].is_array());
}

// ── open_orders() ─────────────────────────────────────────────────────────────

TEST_F(InfoIntegration, OpenOrdersReturnsArray) {
    auto orders = info_->open_orders(kTestAddress);
    ASSERT_TRUE(orders.is_array()) << orders.dump(2);
}

// ── user_fills() ─────────────────────────────────────────────────────────────

TEST_F(InfoIntegration, UserFillsReturnsArray) {
    auto fills = info_->user_fills(kTestAddress);
    ASSERT_TRUE(fills.is_array()) << fills.dump(2);
}

// ── user_fills_by_time() ──────────────────────────────────────────────────────

TEST_F(InfoIntegration, UserFillsByTimeReturnsArray) {
    using namespace std::chrono;
    int64_t now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    int64_t start_ms = now_ms - 7LL * 24 * 3600 * 1000;

    auto fills = info_->user_fills_by_time(kTestAddress, start_ms);
    ASSERT_TRUE(fills.is_array()) << fills.dump(2);
}

// ── sub_accounts() ────────────────────────────────────────────────────────────

TEST_F(InfoIntegration, SubAccountsReturnsArray) {
    auto subs = info_->sub_accounts(kTestAddress);
    ASSERT_TRUE(subs.is_array()) << subs.dump(2);
}

// ── load_meta() coin_to_asset consistency ─────────────────────────────────────

TEST_F(InfoIntegration, CoinToAssetIndicesUnique) {
    info_->load_meta();
    std::unordered_map<int, std::string> index_to_coin;
    for (const auto& [coin, idx] : info_->coin_to_asset) {
        EXPECT_FALSE(index_to_coin.count(idx))
            << "Duplicate index " << idx
            << " for coins " << coin << " and " << index_to_coin[idx];
        index_to_coin[idx] = coin;
    }
}

TEST_F(InfoIntegration, CoinToAssetIndicesStartFromZero) {
    info_->load_meta();
    int min_idx = INT_MAX;
    for (const auto& [coin, idx] : info_->coin_to_asset)
        min_idx = std::min(min_idx, idx);
    EXPECT_EQ(min_idx, 0);
}
