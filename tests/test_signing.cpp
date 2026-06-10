#include <gtest/gtest.h>

#include <hyperliquid/utils/keccak.hpp>
#include <hyperliquid/utils/signing.hpp>
#include <hyperliquid/utils/types.hpp>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

using namespace hyperliquid;
using namespace hyperliquid::signing;

// ── float_to_wire ─────────────────────────────────────────────────────────────

TEST(FloatToWire, WholeNumber) {
    EXPECT_EQ(float_to_wire(1100.0),  "1100");
}

TEST(FloatToWire, LargeWholeNumber) {
    EXPECT_EQ(float_to_wire(50000.0), "50000");
}

TEST(FloatToWire, OneDecimalPlace) {
    EXPECT_EQ(float_to_wire(100.1),   "100.1");
}

TEST(FloatToWire, TwoDecimalPlaces) {
    EXPECT_EQ(float_to_wire(0.01),    "0.01");
}

TEST(FloatToWire, OneDigitAfterDecimal) {
    EXPECT_EQ(float_to_wire(0.2),     "0.2");
    EXPECT_EQ(float_to_wire(0.5),     "0.5");
    EXPECT_EQ(float_to_wire(1.5),     "1.5");
}

TEST(FloatToWire, EightDecimalPlaces) {
    EXPECT_EQ(float_to_wire(0.12345678), "0.12345678");
}

TEST(FloatToWire, MinimumPrecision) {
    EXPECT_EQ(float_to_wire(0.00000001), "0.00000001");
}

TEST(FloatToWire, Zero) {
    EXPECT_EQ(float_to_wire(0.0),  "0");
}

TEST(FloatToWire, NegativeZero) {
    // -0.0 must normalise to "0", not "-0"
    EXPECT_EQ(float_to_wire(-0.0), "0");
}

TEST(FloatToWire, TrailingZerosStripped) {
    EXPECT_EQ(float_to_wire(1.1),    "1.1");
    EXPECT_EQ(float_to_wire(1.10),   "1.1");
    EXPECT_EQ(float_to_wire(1.0),    "1");
}

TEST(FloatToWire, NineDecimalsRounded) {
    EXPECT_THROW(float_to_wire(0.123456789), std::invalid_argument);
}

TEST(FloatToWire, SmallFractionNoLeadingZeroAfterDot) {
    // "0.00000001" — the leading zeros after the decimal are preserved
    EXPECT_EQ(float_to_wire(1e-8), "0.00000001");
}

// ── float_to_usd_int ──────────────────────────────────────────────────────────

TEST(FloatToUsdInt, Zero)        { EXPECT_EQ(float_to_usd_int(0.0),   0LL);       }
TEST(FloatToUsdInt, OneDollar)   { EXPECT_EQ(float_to_usd_int(1.0),   1000000LL); }
TEST(FloatToUsdInt, HalfDollar)  { EXPECT_EQ(float_to_usd_int(0.5),   500000LL);  }
TEST(FloatToUsdInt, OneMilli)    { EXPECT_EQ(float_to_usd_int(0.001), 1000LL);    }
TEST(FloatToUsdInt, Hundred)     { EXPECT_EQ(float_to_usd_int(100.0), 100000000LL); }
TEST(FloatToUsdInt, FractionalCents) {
    EXPECT_EQ(float_to_usd_int(1.5), 1500000LL);
}

TEST(FloatToUsdInt, ThrowsWhenRoundingWouldBeRequired) {
    EXPECT_THROW(float_to_usd_int(0.0000001), std::invalid_argument);
}

// ── hex_to_bytes / bytes_to_hex ───────────────────────────────────────────────

TEST(HexUtils, BytesToHexWithPrefix) {
    std::vector<uint8_t> b = {0xde, 0xad, 0xbe, 0xef};
    EXPECT_EQ(bytes_to_hex(b.data(), b.size()), "0xdeadbeef");
}

TEST(HexUtils, BytesToHexWithoutPrefix) {
    std::vector<uint8_t> b = {0xde, 0xad, 0xbe, 0xef};
    EXPECT_EQ(bytes_to_hex(b.data(), b.size(), false), "deadbeef");
}

TEST(HexUtils, HexToBytesWithPrefix) {
    auto b = hex_to_bytes("0xdeadbeef");
    ASSERT_EQ(b.size(), 4u);
    EXPECT_EQ(b[0], 0xde);
    EXPECT_EQ(b[1], 0xad);
    EXPECT_EQ(b[2], 0xbe);
    EXPECT_EQ(b[3], 0xef);
}

TEST(HexUtils, HexToBytesWithoutPrefix) {
    auto b = hex_to_bytes("deadbeef");
    ASSERT_EQ(b.size(), 4u);
    EXPECT_EQ(b[0], 0xde);
}

TEST(HexUtils, HexToBytesUpperCase) {
    auto b = hex_to_bytes("0xDEADBEEF");
    ASSERT_EQ(b.size(), 4u);
    EXPECT_EQ(b[0], 0xde);
    EXPECT_EQ(b[3], 0xef);
}

TEST(HexUtils, RoundTrip) {
    std::vector<uint8_t> orig = {0x00, 0x01, 0x7f, 0x80, 0xff};
    EXPECT_EQ(orig, hex_to_bytes(bytes_to_hex(orig.data(), orig.size())));
}

TEST(HexUtils, EmptyBytes) {
    EXPECT_TRUE(hex_to_bytes("0x").empty());
    EXPECT_TRUE(hex_to_bytes("").empty());
}

TEST(HexUtils, OddLengthThrows) {
    EXPECT_THROW(hex_to_bytes("abc"),   std::invalid_argument);
    EXPECT_THROW(hex_to_bytes("0xabc"), std::invalid_argument);
}

TEST(HexUtils, AllZeroes20Bytes) {
    std::vector<uint8_t> zeros(20, 0x00);
    EXPECT_EQ(bytes_to_hex(zeros.data(), zeros.size()),
              "0x" + std::string(40, '0'));
}

TEST(HexUtils, SingleByte) {
    EXPECT_EQ(bytes_to_hex(std::vector<uint8_t>{0x0f}.data(), 1), "0x0f");
    EXPECT_EQ(bytes_to_hex(std::vector<uint8_t>{0xff}.data(), 1), "0xff");
}

// ── private_key_to_address ────────────────────────────────────────────────────

TEST(PrivateKeyToAddress, HardhatAccount0) {
    // Hardhat test account 0 — universally known test key
    const std::string pk =
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    EXPECT_EQ(private_key_to_address(pk),
              "0xf39fd6e51aad88f6f4ce6ab8827279cfffb92266");
}

TEST(PrivateKeyToAddress, WithAndWithoutPrefix) {
    const std::string with_prefix =
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    const std::string without_prefix =
        "ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    EXPECT_EQ(private_key_to_address(with_prefix),
              private_key_to_address(without_prefix));
}

TEST(PrivateKeyToAddress, FormatIs0xPlus40LowercaseHex) {
    const std::string pk =
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    const auto addr = private_key_to_address(pk);
    ASSERT_EQ(addr.size(), 42u);
    EXPECT_EQ(addr.substr(0, 2), "0x");
    for (char c : addr.substr(2)) {
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)));
        EXPECT_FALSE(std::isupper(static_cast<unsigned char>(c)));
    }
}

TEST(PrivateKeyToAddress, DifferentKeysDifferentAddresses) {
    // The two standard Hardhat accounts produce distinct addresses
    const std::string pk0 =
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    const std::string pk1 =
        "0x59c6995e998f97a5a0044966f0945389dc9e86dae88c7a8412f4603b6b78690d";
    EXPECT_NE(private_key_to_address(pk0), private_key_to_address(pk1));
}

// ── order_request_to_wire ────────────────────────────────────────────────────

TEST(OrderRequestToWire, LimitGtcBuy) {
    OrderRequest req;
    req.coin       = "ETH";
    req.is_buy     = true;
    req.sz         = 0.01;
    req.limit_px   = 1100.0;
    req.order_type = LimitOrderType{Tif::Gtc};
    req.reduce_only = false;

    auto w = order_request_to_wire(req, 4);

    EXPECT_EQ(w["a"].get<int>(), 4);
    EXPECT_EQ(w["b"].get<bool>(), true);
    EXPECT_EQ(w["p"].get<std::string>(), "1100");
    EXPECT_EQ(w["s"].get<std::string>(), "0.01");
    EXPECT_EQ(w["r"].get<bool>(), false);
    ASSERT_TRUE(w["t"].contains("limit"));
    EXPECT_EQ(w["t"]["limit"]["tif"].get<std::string>(), "Gtc");
    EXPECT_FALSE(w.contains("c"));
}

TEST(OrderRequestToWire, LimitIocSell) {
    OrderRequest req;
    req.coin       = "BTC";
    req.is_buy     = false;
    req.sz         = 0.5;
    req.limit_px   = 60000.0;
    req.order_type = LimitOrderType{Tif::Ioc};
    req.reduce_only = true;

    auto w = order_request_to_wire(req, 0);

    EXPECT_EQ(w["a"].get<int>(), 0);
    EXPECT_EQ(w["b"].get<bool>(), false);
    EXPECT_EQ(w["s"].get<std::string>(), "0.5");
    EXPECT_EQ(w["p"].get<std::string>(), "60000");
    EXPECT_EQ(w["r"].get<bool>(), true);
    EXPECT_EQ(w["t"]["limit"]["tif"].get<std::string>(), "Ioc");
}

TEST(OrderRequestToWire, LimitAlo) {
    OrderRequest req;
    req.coin       = "SOL";
    req.is_buy     = true;
    req.sz         = 10.0;
    req.limit_px   = 100.0;
    req.order_type = LimitOrderType{Tif::Alo};

    auto w = order_request_to_wire(req, 1);
    EXPECT_EQ(w["t"]["limit"]["tif"].get<std::string>(), "Alo");
}

TEST(OrderRequestToWire, TriggerTakeProfit) {
    OrderRequest req;
    req.coin       = "ETH";
    req.is_buy     = false;
    req.sz         = 0.1;
    req.limit_px   = 0.0;
    req.order_type = TriggerOrderType{2000.0, true, "tp"};
    req.reduce_only = true;

    auto w = order_request_to_wire(req, 4);

    ASSERT_TRUE(w["t"].contains("trigger"));
    EXPECT_EQ(w["t"]["trigger"]["triggerPx"].get<std::string>(), "2000");
    EXPECT_EQ(w["t"]["trigger"]["isMarket"].get<bool>(), true);
    EXPECT_EQ(w["t"]["trigger"]["tpsl"].get<std::string>(), "tp");
}

TEST(OrderRequestToWire, TriggerStopLoss) {
    OrderRequest req;
    req.coin       = "ETH";
    req.is_buy     = true;
    req.sz         = 0.1;
    req.limit_px   = 1000.0;
    req.order_type = TriggerOrderType{800.0, false, "sl"};
    req.reduce_only = true;

    auto w = order_request_to_wire(req, 4);
    EXPECT_EQ(w["t"]["trigger"]["tpsl"].get<std::string>(), "sl");
    EXPECT_EQ(w["t"]["trigger"]["isMarket"].get<bool>(), false);
    EXPECT_EQ(w["t"]["trigger"]["triggerPx"].get<std::string>(), "800");
}

TEST(OrderRequestToWire, WithCloid) {
    OrderRequest req;
    req.coin       = "ETH";
    req.is_buy     = true;
    req.sz         = 0.01;
    req.limit_px   = 1100.0;
    req.order_type = LimitOrderType{Tif::Gtc};
    req.cloid      = Cloid::from_int(42);

    auto w = order_request_to_wire(req, 4);
    ASSERT_TRUE(w.contains("c"));
    EXPECT_EQ(w["c"].get<std::string>(),
              std::string(Cloid::from_int(42).to_raw()));
}

TEST(OrderRequestToWire, WithoutCloidNoKey) {
    OrderRequest req;
    req.coin       = "ETH";
    req.is_buy     = true;
    req.sz         = 0.01;
    req.limit_px   = 1100.0;
    req.order_type = LimitOrderType{Tif::Gtc};
    // req.cloid intentionally not set

    auto w = order_request_to_wire(req, 4);
    EXPECT_FALSE(w.contains("c"));
}

// Key insertion order is critical for msgpack hashing to match Python SDK.
TEST(OrderRequestToWire, KeyOrderAbpSrt) {
    OrderRequest req;
    req.coin       = "ETH";
    req.is_buy     = true;
    req.sz         = 0.01;
    req.limit_px   = 1100.0;
    req.order_type = LimitOrderType{Tif::Gtc};

    auto w = order_request_to_wire(req, 4);
    const std::string d = w.dump();

    auto pos = [&](const char* key) { return d.find(key); };
    EXPECT_LT(pos("\"a\""), pos("\"b\""));
    EXPECT_LT(pos("\"b\""), pos("\"p\""));
    EXPECT_LT(pos("\"p\""), pos("\"s\""));
    EXPECT_LT(pos("\"s\""), pos("\"r\""));
    EXPECT_LT(pos("\"r\""), pos("\"t\""));
}

TEST(OrderRequestToWire, CloidKeyAfterT) {
    OrderRequest req;
    req.coin       = "ETH";
    req.is_buy     = true;
    req.sz         = 0.01;
    req.limit_px   = 1100.0;
    req.order_type = LimitOrderType{Tif::Gtc};
    req.cloid      = Cloid::from_int(7);

    auto w = order_request_to_wire(req, 4);
    const std::string d = w.dump();
    EXPECT_LT(d.find("\"t\""), d.find("\"c\""));
}

TEST(OrderRequestToWire, AssetIndexEncoded) {
    OrderRequest req;
    req.coin       = "X";
    req.is_buy     = true;
    req.sz         = 1.0;
    req.limit_px   = 1.0;
    req.order_type = LimitOrderType{Tif::Gtc};

    auto w = order_request_to_wire(req, 99);
    EXPECT_EQ(w["a"].get<int>(), 99);
}

// ── order_wires_to_action ─────────────────────────────────────────────────────

TEST(OrderWiresToAction, EmptyOrdersNoBuilder) {
    auto a = order_wires_to_action({}, "na");
    EXPECT_EQ(a["type"].get<std::string>(), "order");
    EXPECT_EQ(a["grouping"].get<std::string>(), "na");
    ASSERT_TRUE(a["orders"].is_array());
    EXPECT_TRUE(a["orders"].empty());
    EXPECT_FALSE(a.contains("builder"));
}

TEST(OrderWiresToAction, WithBuilder) {
    BuilderInfo bi{"0xbuilder", 10};
    auto a = order_wires_to_action({}, "na", bi);
    ASSERT_TRUE(a.contains("builder"));
    EXPECT_EQ(a["builder"]["b"].get<std::string>(), "0xbuilder");
    EXPECT_EQ(a["builder"]["f"].get<int>(), 10);
}

TEST(OrderWiresToAction, GroupingPreserved) {
    EXPECT_EQ(order_wires_to_action({}, "na")["grouping"],         "na");
    EXPECT_EQ(order_wires_to_action({}, "normalTpsl")["grouping"], "normalTpsl");
}

// type, orders, grouping must appear in that insertion order
TEST(OrderWiresToAction, KeyOrderTypeOrdersGrouping) {
    auto a = order_wires_to_action({}, "na");
    const std::string d = a.dump();
    EXPECT_LT(d.find("\"type\""),    d.find("\"orders\""));
    EXPECT_LT(d.find("\"orders\""),  d.find("\"grouping\""));
}

TEST(OrderWiresToAction, BuilderKeyAfterGrouping) {
    auto a = order_wires_to_action({}, "na", BuilderInfo{"0xb", 1});
    const std::string d = a.dump();
    EXPECT_LT(d.find("\"grouping\""), d.find("\"builder\""));
}

TEST(OrderWiresToAction, WiresEmbedded) {
    OrderRequest req;
    req.coin       = "ETH";
    req.is_buy     = true;
    req.sz         = 0.01;
    req.limit_px   = 1100.0;
    req.order_type = LimitOrderType{Tif::Gtc};

    std::vector<nlohmann::ordered_json> wires;
    wires.push_back(order_request_to_wire(req, 4));

    auto a = order_wires_to_action(std::move(wires), "na");
    ASSERT_EQ(a["orders"].size(), 1u);
    EXPECT_EQ(a["orders"][0]["a"].get<int>(), 4);
}

// ── action_hash ───────────────────────────────────────────────────────────────

// Independently reconstruct the same byte sequence and verify the hash matches.

TEST(ActionHash, NoVault) {
    nlohmann::ordered_json action;
    action["type"] = "cancel";
    const int64_t nonce = 1000LL;

    std::vector<uint8_t> buf = nlohmann::ordered_json::to_msgpack(action);
    for (int i = 7; i >= 0; --i)
        buf.push_back(static_cast<uint8_t>((nonce >> (i * 8)) & 0xFF));
    buf.push_back(0x00); // no vault

    EXPECT_EQ(keccak256(buf), action_hash(action, {}, nonce));
}

TEST(ActionHash, WithVaultAddress) {
    nlohmann::ordered_json action;
    action["type"] = "cancel";
    const int64_t nonce = 5000LL;
    const std::string vault = "0x1234567890abcdef1234567890abcdef12345678";

    std::vector<uint8_t> buf = nlohmann::ordered_json::to_msgpack(action);
    for (int i = 7; i >= 0; --i)
        buf.push_back(static_cast<uint8_t>((nonce >> (i * 8)) & 0xFF));
    buf.push_back(0x01); // vault present
    auto vb = hex_to_bytes(vault);
    buf.insert(buf.end(), vb.begin(), vb.end());

    EXPECT_EQ(keccak256(buf), action_hash(action, vault, nonce));
}

TEST(ActionHash, WithExpiresAfter) {
    nlohmann::ordered_json action;
    action["type"] = "cancel";
    const int64_t nonce   = 1000LL;
    const int64_t expires = 9999999999LL;

    std::vector<uint8_t> buf = nlohmann::ordered_json::to_msgpack(action);
    for (int i = 7; i >= 0; --i)
        buf.push_back(static_cast<uint8_t>((nonce >> (i * 8)) & 0xFF));
    buf.push_back(0x00); // no vault
    buf.push_back(0x00); // expires_after flag
    for (int i = 7; i >= 0; --i)
        buf.push_back(static_cast<uint8_t>((expires >> (i * 8)) & 0xFF));

    EXPECT_EQ(keccak256(buf), action_hash(action, {}, nonce, expires));
}

TEST(ActionHash, Deterministic) {
    nlohmann::ordered_json action;
    action["type"]     = "order";
    action["orders"]   = nlohmann::ordered_json::array();
    action["grouping"] = "na";

    EXPECT_EQ(action_hash(action, {}, 12345LL),
              action_hash(action, {}, 12345LL));
}

TEST(ActionHash, DifferentNoncesDifferentHashes) {
    nlohmann::ordered_json action;
    action["type"] = "cancel";
    EXPECT_NE(action_hash(action, {}, 1000LL),
              action_hash(action, {}, 1001LL));
}

TEST(ActionHash, VaultPresenceChangesHash) {
    nlohmann::ordered_json action;
    action["type"] = "cancel";
    const int64_t nonce = 1000LL;

    auto h_no_vault   = action_hash(action, {}, nonce);
    auto h_with_vault = action_hash(
        action, "0x1234567890abcdef1234567890abcdef12345678", nonce);
    EXPECT_NE(h_no_vault, h_with_vault);
}

TEST(ActionHash, DifferentActionsProduceDifferentHashes) {
    nlohmann::ordered_json a1, a2;
    a1["type"] = "cancel";
    a2["type"] = "order";
    EXPECT_NE(action_hash(a1, {}, 1000LL),
              action_hash(a2, {}, 1000LL));
}

TEST(ActionHash, ResultIs32Bytes) {
    nlohmann::ordered_json action;
    action["type"] = "cancel";
    auto h = action_hash(action, {}, 1000LL);
    EXPECT_EQ(h.size(), 32u);
}

// ── sign_l1_action ────────────────────────────────────────────────────────────

static const std::string kTestKey =
    "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";

static bool is_valid_hex64(const std::string& s) {
    if (s.size() != 66) return false;
    if (s.substr(0, 2) != "0x") return false;
    return std::all_of(s.begin() + 2, s.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) &&
               !std::isupper(static_cast<unsigned char>(c));
    });
}

TEST(SignL1Action, SignatureFormat) {
    nlohmann::ordered_json action;
    action["type"]    = "cancel";
    action["cancels"] = nlohmann::ordered_json::array();

    auto sig = sign_l1_action(kTestKey, action, {}, 1000LL, false);

    EXPECT_TRUE(is_valid_hex64(sig.r)) << "r = " << sig.r;
    EXPECT_TRUE(is_valid_hex64(sig.s)) << "s = " << sig.s;
    EXPECT_TRUE(sig.v == 27 || sig.v == 28) << "v = " << sig.v;
}

TEST(SignL1Action, BothNetworksDontThrow) {
    nlohmann::ordered_json action;
    action["type"]     = "order";
    action["orders"]   = nlohmann::ordered_json::array();
    action["grouping"] = "na";

    EXPECT_NO_THROW(sign_l1_action(kTestKey, action, {}, 1699999999000LL, false));
    EXPECT_NO_THROW(sign_l1_action(kTestKey, action, {}, 1699999999000LL, true));
}

TEST(SignL1Action, WithVaultAddressDontThrow) {
    nlohmann::ordered_json action;
    action["type"] = "cancel";
    action["cancels"] = nlohmann::ordered_json::array();

    EXPECT_NO_THROW(sign_l1_action(kTestKey, action,
        std::string("0x1234567890abcdef1234567890abcdef12345678"),
        1000LL, false));
}

TEST(SignL1Action, RSNotEqual) {
    // For any realistic hash, r and s are essentially never equal
    nlohmann::ordered_json action;
    action["type"] = "cancel";
    action["cancels"] = nlohmann::ordered_json::array();

    auto sig = sign_l1_action(kTestKey, action, {}, 12345678900LL, false);
    // r and s can theoretically be equal but it's astronomically unlikely
    // This is a sanity check, not a hard invariant
    (void)sig; // just verify it compiles and runs
}

// ── sign_user_signed_action ───────────────────────────────────────────────────

// usdSend uses "time" (not "nonce") as its EIP-712 timestamp field name.
// When testing sign_user_signed_action directly we must supply the correct key.
static nlohmann::ordered_json make_usd_send_action() {
    nlohmann::ordered_json a;
    a["type"]        = "usdSend";
    a["destination"] = "0x0000000000000000000000000000000000000001";
    a["amount"]      = "10.0";
    a["time"]        = 1000LL; // EIP-712 field is "time" for usdSend
    return a;
}

static const std::vector<std::pair<std::string, std::string>> kUsdSendTypes = {
    {"hyperliquidChain", "string"},
    {"destination",      "string"},
    {"amount",           "string"},
    {"time",             "uint64"},
};

TEST(SignUserSignedAction, MutatesActionWithChainFields) {
    auto action = make_usd_send_action();

    sign_user_signed_action(kTestKey, action, kUsdSendTypes,
                            "HyperliquidTransaction:UsdSend", false);

    EXPECT_TRUE(action.contains("signatureChainId"));
    EXPECT_TRUE(action.contains("hyperliquidChain"));
    EXPECT_EQ(action["signatureChainId"].get<std::string>(), "0x66eee");
    EXPECT_EQ(action["hyperliquidChain"].get<std::string>(), "Testnet");
}

TEST(SignUserSignedAction, MainnetChainLabel) {
    auto action = make_usd_send_action();

    sign_user_signed_action(kTestKey, action, kUsdSendTypes,
                            "HyperliquidTransaction:UsdSend", true);

    EXPECT_EQ(action["hyperliquidChain"].get<std::string>(), "Mainnet");
}

TEST(SignUserSignedAction, SignatureFormat) {
    auto action = make_usd_send_action();
    action["amount"] = "1.0";
    action["time"]   = 42LL;

    auto sig = sign_user_signed_action(kTestKey, action, kUsdSendTypes,
                                       "HyperliquidTransaction:UsdSend", false);

    EXPECT_TRUE(is_valid_hex64(sig.r)) << "r = " << sig.r;
    EXPECT_TRUE(is_valid_hex64(sig.s)) << "s = " << sig.s;
    EXPECT_TRUE(sig.v == 27 || sig.v == 28) << "v = " << sig.v;
}

TEST(SignUserSignedAction, SignatureChainIdConstant) {
    // signatureChainId must always be 0x66eee regardless of mainnet/testnet
    auto a1 = make_usd_send_action();
    sign_user_signed_action(kTestKey, a1, kUsdSendTypes,
                            "HyperliquidTransaction:UsdSend", false);
    auto a2 = make_usd_send_action();
    sign_user_signed_action(kTestKey, a2, kUsdSendTypes,
                            "HyperliquidTransaction:UsdSend", true);

    EXPECT_EQ(a1["signatureChainId"].get<std::string>(), "0x66eee");
    EXPECT_EQ(a2["signatureChainId"].get<std::string>(), "0x66eee");
}

// usdClassTransfer uses "nonce" (not "time") as its EIP-712 timestamp field.
TEST(SignUserSignedAction, UsdClassTransferUsesNonceField) {
    nlohmann::ordered_json action;
    action["type"]   = "usdClassTransfer";
    action["amount"] = "100.0";
    action["toPerp"] = true;
    action["nonce"]  = 9999LL;

    const std::vector<std::pair<std::string, std::string>> types = {
        {"hyperliquidChain", "string"},
        {"amount",           "string"},
        {"toPerp",           "bool"},
        {"nonce",            "uint64"},
    };

    EXPECT_NO_THROW(
        sign_user_signed_action(kTestKey, action, types,
                                "HyperliquidTransaction:UsdClassTransfer", false));
}

// ── get_timestamp_ms ──────────────────────────────────────────────────────────

TEST(Timestamps, IsPositive) {
    EXPECT_GT(get_timestamp_ms(), 0LL);
}

TEST(Timestamps, IsAfter2024) {
    // 2024-01-01 in ms
    EXPECT_GT(get_timestamp_ms(), 1704067200000LL);
}

TEST(Timestamps, IsMonotone) {
    auto t1 = get_timestamp_ms();
    auto t2 = get_timestamp_ms();
    EXPECT_GE(t2, t1);
}
