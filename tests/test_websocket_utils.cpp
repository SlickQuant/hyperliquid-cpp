#include <gtest/gtest.h>
#include <hyperliquid/websocket_manager.hpp>

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

using hyperliquid::WebsocketManager;
using json = nlohmann::json;

// ── http_to_ws_url ────────────────────────────────────────────────────────────

TEST(HttpToWsUrl, MainnetHttps) {
    EXPECT_EQ(WebsocketManager::http_to_ws_url("https://api.hyperliquid.xyz"),
              "wss://api.hyperliquid.xyz/ws");
}

TEST(HttpToWsUrl, TestnetHttps) {
    EXPECT_EQ(
        WebsocketManager::http_to_ws_url("https://api.hyperliquid-testnet.xyz"),
        "wss://api.hyperliquid-testnet.xyz/ws");
}

TEST(HttpToWsUrl, LocalhostHttp) {
    EXPECT_EQ(WebsocketManager::http_to_ws_url("http://localhost:3001"),
              "ws://localhost:3001/ws");
}

TEST(HttpToWsUrl, TrailingSlashStripped) {
    EXPECT_EQ(WebsocketManager::http_to_ws_url("https://api.hyperliquid.xyz/"),
              "wss://api.hyperliquid.xyz/ws");
}

TEST(HttpToWsUrl, MultipleTrailingSlashesStripped) {
    EXPECT_EQ(WebsocketManager::http_to_ws_url("https://api.hyperliquid.xyz///"),
              "wss://api.hyperliquid.xyz/ws");
}

TEST(HttpToWsUrl, AppendsSingleWsPath) {
    // Must not double the /ws path
    std::string result = WebsocketManager::http_to_ws_url("https://api.hyperliquid.xyz");
    EXPECT_EQ(result.substr(result.size() - 3), "/ws");
    size_t count = 0;
    for (size_t p = result.find("/ws"); p != std::string::npos; p = result.find("/ws", p + 1))
        ++count;
    EXPECT_EQ(count, 1u);
}

// ── to_identifier — channel types without coin/user suffix ───────────────────

TEST(ToIdentifier, AllMids) {
    EXPECT_EQ(WebsocketManager::to_identifier({{"type", "allMids"}}),
              "allMids");
}

// ── to_identifier — coin-based subscriptions ─────────────────────────────────

TEST(ToIdentifier, L2BookEth) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "l2Book"}, {"coin", "ETH"}}),
              "l2Book:ETH");
}

TEST(ToIdentifier, L2BookBtc) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "l2Book"}, {"coin", "BTC"}}),
              "l2Book:BTC");
}

TEST(ToIdentifier, BboSol) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "bbo"}, {"coin", "SOL"}}),
              "bbo:SOL");
}

TEST(ToIdentifier, TradesArb) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "trades"}, {"coin", "ARB"}}),
              "trades:ARB");
}

TEST(ToIdentifier, ActiveAssetCtx) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "activeAssetCtx"}, {"coin", "ETH"}}),
              "activeAssetCtx:ETH");
}

// ── to_identifier — candle (coin + interval) ──────────────────────────────────

TEST(ToIdentifier, Candle5m) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "candle"}, {"coin", "BTC"}, {"interval", "5m"}}),
              "candle:BTC,5m");
}

TEST(ToIdentifier, Candle1h) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "candle"}, {"coin", "ETH"}, {"interval", "1h"}}),
              "candle:ETH,1h");
}

TEST(ToIdentifier, Candle1d) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "candle"}, {"coin", "SOL"}, {"interval", "1d"}}),
              "candle:SOL,1d");
}

TEST(ToIdentifier, Candle1m) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "candle"}, {"coin", "AVAX"}, {"interval", "1m"}}),
              "candle:AVAX,1m");
}

// ── to_identifier — user-based subscriptions ─────────────────────────────────

TEST(ToIdentifier, UserEvents) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "userEvents"}, {"user", "0xabc"}}),
              "userEvents:0xabc");
}

TEST(ToIdentifier, UserFills) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "userFills"}, {"user", "0xf39fd6"}}),
              "userFills:0xf39fd6");
}

TEST(ToIdentifier, OrderUpdates) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "orderUpdates"}, {"user", "0xdeadbeef"}}),
              "orderUpdates:0xdeadbeef");
}

TEST(ToIdentifier, UserFundings) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "userFundings"}, {"user", "0xabc123"}}),
              "userFundings:0xabc123");
}

TEST(ToIdentifier, UserNonFundingLedgerUpdates) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "userNonFundingLedgerUpdates"}, {"user", "0xaaa"}}),
              "userNonFundingLedgerUpdates:0xaaa");
}

TEST(ToIdentifier, WebData2) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "webData2"}, {"user", "0xbbb"}}),
              "webData2:0xbbb");
}

// ── to_identifier — activeAssetData (coin + user) ────────────────────────────

TEST(ToIdentifier, ActiveAssetData) {
    EXPECT_EQ(WebsocketManager::to_identifier(
                  {{"type", "activeAssetData"},
                   {"coin", "ETH"}, {"user", "0xaaa"}}),
              "activeAssetData:ETH,0xaaa");
}

// ── to_identifier — coin values are preserved verbatim ───────────────────────

TEST(ToIdentifier, CoinCaseSensitive) {
    // Identifiers must preserve the original coin string
    EXPECT_NE(WebsocketManager::to_identifier(
                  {{"type", "l2Book"}, {"coin", "eth"}}),
              WebsocketManager::to_identifier(
                  {{"type", "l2Book"}, {"coin", "ETH"}}));
}

TEST(ToIdentifier, DifferentUsersDifferentIdentifiers) {
    auto id1 = WebsocketManager::to_identifier(
        {{"type", "userFills"}, {"user", "0x111"}});
    auto id2 = WebsocketManager::to_identifier(
        {{"type", "userFills"}, {"user", "0x222"}});
    EXPECT_NE(id1, id2);
}

TEST(ToIdentifier, DifferentTypesDifferentIdentifiers) {
    auto id_l2  = WebsocketManager::to_identifier(
        {{"type", "l2Book"}, {"coin", "ETH"}});
    auto id_bbo = WebsocketManager::to_identifier(
        {{"type", "bbo"},    {"coin", "ETH"}});
    EXPECT_NE(id_l2, id_bbo);
}
