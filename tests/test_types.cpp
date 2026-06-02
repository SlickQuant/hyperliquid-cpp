#include <gtest/gtest.h>
#include <hyperliquid/utils/types.hpp>

#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

using namespace hyperliquid;

// ── Tif ───────────────────────────────────────────────────────────────────────

TEST(TifToString, Alo) { EXPECT_EQ(tif_to_string(Tif::Alo), "Alo"); }
TEST(TifToString, Ioc) { EXPECT_EQ(tif_to_string(Tif::Ioc), "Ioc"); }
TEST(TifToString, Gtc) { EXPECT_EQ(tif_to_string(Tif::Gtc), "Gtc"); }

// ── Cloid construction ────────────────────────────────────────────────────────

TEST(Cloid, FromIntZero) {
    auto c = Cloid::from_int(0);
    EXPECT_EQ(c.to_raw(), "0x00000000000000000000000000000000");
}

TEST(Cloid, FromIntOne) {
    auto c = Cloid::from_int(1);
    EXPECT_EQ(c.to_raw(), "0x00000000000000000000000000000001");
}

TEST(Cloid, FromIntMax) {
    // UINT64_MAX = 0xffffffffffffffff, padded to 32 hex chars
    auto c = Cloid::from_int(std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(c.to_raw(), "0x0000000000000000ffffffffffffffff");
}

TEST(Cloid, FromIntArbitrary) {
    auto c = Cloid::from_int(0x0102030405060708ULL);
    EXPECT_EQ(c.to_raw(), "0x00000000000000000102030405060708");
}

TEST(Cloid, FromIntSmall) {
    auto c = Cloid::from_int(255);
    EXPECT_EQ(c.to_raw(), "0x000000000000000000000000000000ff");
}

TEST(Cloid, FromStr) {
    const std::string raw = "0x0102030405060708090a0b0c0d0e0f10";
    auto c = Cloid::from_str(raw);
    EXPECT_EQ(c.to_raw(), raw);
}

TEST(Cloid, DirectConstructionValid) {
    Cloid c("0xdeadbeefdeadbeefdeadbeefdeadbeef");
    EXPECT_EQ(c.to_raw(), "0xdeadbeefdeadbeefdeadbeefdeadbeef");
}

TEST(Cloid, ToRawLength) {
    // Must always be exactly 34 chars: "0x" + 32 hex
    auto c = Cloid::from_int(12345);
    EXPECT_EQ(c.to_raw().size(), 34u);
}

// ── Cloid equality ────────────────────────────────────────────────────────────

TEST(Cloid, EqualWhenSameValue) {
    auto c1 = Cloid::from_int(42);
    auto c2 = Cloid::from_int(42);
    EXPECT_EQ(c1, c2);
}

TEST(Cloid, NotEqualWhenDifferent) {
    auto c1 = Cloid::from_int(1);
    auto c2 = Cloid::from_int(2);
    EXPECT_NE(c1, c2);
}

TEST(Cloid, EqualViaFromStr) {
    auto c1 = Cloid::from_int(0xABCDEF);
    auto c2 = Cloid::from_str(std::string(c1.to_raw()));
    EXPECT_EQ(c1, c2);
}

// ── Cloid validation ──────────────────────────────────────────────────────────

TEST(Cloid, InvalidNoPrefixThrows) {
    EXPECT_THROW(Cloid("deadbeefdeadbeefdeadbeefdeadbeef"),
                 std::invalid_argument);
}

TEST(Cloid, InvalidTooShortThrows) {
    EXPECT_THROW(Cloid("0xdeadbeef"), std::invalid_argument);
}

TEST(Cloid, InvalidTooLongThrows) {
    // 33 hex chars after 0x
    EXPECT_THROW(Cloid("0xdeadbeefdeadbeefdeadbeefdeadbeef0"),
                 std::invalid_argument);
}

TEST(Cloid, InvalidEmptyThrows) {
    EXPECT_THROW(Cloid(""), std::invalid_argument);
}

TEST(Cloid, InvalidOnlyPrefixThrows) {
    EXPECT_THROW(Cloid("0x"), std::invalid_argument);
}

// ── OrderRequest defaults ─────────────────────────────────────────────────────

TEST(OrderRequest, DefaultIsBuy) {
    OrderRequest req;
    EXPECT_TRUE(req.is_buy);
}

TEST(OrderRequest, DefaultSzZero) {
    OrderRequest req;
    EXPECT_EQ(req.sz, 0.0);
}

TEST(OrderRequest, DefaultLimitPxZero) {
    OrderRequest req;
    EXPECT_EQ(req.limit_px, 0.0);
}

TEST(OrderRequest, DefaultReduceOnlyFalse) {
    OrderRequest req;
    EXPECT_FALSE(req.reduce_only);
}

TEST(OrderRequest, DefaultNoCloid) {
    OrderRequest req;
    EXPECT_FALSE(req.cloid.has_value());
}

TEST(OrderRequest, DefaultOrderTypeIsLimitGtc) {
    OrderRequest req;
    ASSERT_TRUE(std::holds_alternative<LimitOrderType>(req.order_type));
    EXPECT_EQ(std::get<LimitOrderType>(req.order_type).tif, Tif::Gtc);
}

// ── LimitOrderType ────────────────────────────────────────────────────────────

TEST(LimitOrderType, DefaultTifIsGtc) {
    LimitOrderType lot;
    EXPECT_EQ(lot.tif, Tif::Gtc);
}

TEST(LimitOrderType, IocConstruction) {
    LimitOrderType lot{Tif::Ioc};
    EXPECT_EQ(lot.tif, Tif::Ioc);
}

// ── TriggerOrderType ──────────────────────────────────────────────────────────

TEST(TriggerOrderType, TpConstruction) {
    TriggerOrderType tot{1800.0, true, "tp"};
    EXPECT_EQ(tot.trigger_px, 1800.0);
    EXPECT_EQ(tot.is_market, true);
    EXPECT_EQ(tot.tpsl, "tp");
}

TEST(TriggerOrderType, SlConstruction) {
    TriggerOrderType tot{900.0, false, "sl"};
    EXPECT_EQ(tot.trigger_px, 900.0);
    EXPECT_EQ(tot.is_market, false);
    EXPECT_EQ(tot.tpsl, "sl");
}

// ── OrderType variant ─────────────────────────────────────────────────────────

TEST(OrderTypeVariant, HoldsLimit) {
    OrderType ot = LimitOrderType{Tif::Gtc};
    EXPECT_TRUE(std::holds_alternative<LimitOrderType>(ot));
    EXPECT_FALSE(std::holds_alternative<TriggerOrderType>(ot));
}

TEST(OrderTypeVariant, HoldsTrigger) {
    OrderType ot = TriggerOrderType{1500.0, true, "tp"};
    EXPECT_FALSE(std::holds_alternative<LimitOrderType>(ot));
    EXPECT_TRUE(std::holds_alternative<TriggerOrderType>(ot));
}

// ── BuilderInfo ───────────────────────────────────────────────────────────────

TEST(BuilderInfo, DefaultFeeIsZero) {
    BuilderInfo bi{"0xabc", 0};
    EXPECT_EQ(bi.f, 0);
    EXPECT_EQ(bi.b, "0xabc");
}

TEST(BuilderInfo, NonZeroFee) {
    BuilderInfo bi{"0xbuilder", 10};
    EXPECT_EQ(bi.f, 10);
}
