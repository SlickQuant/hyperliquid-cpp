#include <gtest/gtest.h>

#include <hyperliquid/utils/l2_diff.hpp>

#include <nlohmann/json.hpp>

#include <stdexcept>

using hyperliquid::decode_l2_diff;
using json = nlohmann::json;

TEST(L2DiffDecoder, DecodesCompressedDiffPayload) {
    const std::string payload =
        "XZI7DgIxDETv4tpC/m2cbMU9VqloKRDQrfbuGAqwKaOXmeTF2eECK5zZBBCesLJ3"
        "JSYSbq4IV1i3bYdb7GksyicChEes6KRE3uDALxVP1CSaCrVESb1zoVTo0iRT7"
        "omauo1COVEqQSqHBpqYbCTneujlViut6l7ua6V4+acjNffOTXNzK66ithS6p"
        "Gy18Rw0iWQOetEJ199kep6MDh6WgyPXEpv2Qst96OMyJ8L9/TOY4kEZFQ0bss"
        "95vAA=";

    const json diff = decode_l2_diff(payload);

    EXPECT_EQ(diff.at("c").get<std::string>(), "@142");
    EXPECT_EQ(diff.at("t").get<std::int64_t>(), 1783010021673LL);

    ASSERT_TRUE(diff.at("l").is_array());
    ASSERT_EQ(diff.at("l").size(), 2u);
    ASSERT_FALSE(diff.at("l").at(0).empty());
    ASSERT_FALSE(diff.at("l").at(1).empty());
    EXPECT_EQ(diff.at("l").at(0).at(0).at("p").get<std::string>(), "61231.0");
    EXPECT_EQ(diff.at("l").at(0).at(0).at("s").get<std::string>(), "0.30076");
    EXPECT_EQ(diff.at("l").at(1).at(0).at("p").get<std::string>(), "61232.0");
    EXPECT_EQ(diff.at("l").at(1).at(0).at("s").get<std::string>(), "0.08123");

    ASSERT_TRUE(diff.at("r").is_array());
    ASSERT_EQ(diff.at("r").size(), 2u);
    EXPECT_EQ(diff.at("r").at(0).at(0).get<int>(), 10);
    EXPECT_EQ(diff.at("r").at(1).at(0).get<int>(), 1);
}

TEST(L2DiffDecoder, RejectsInvalidBase64) {
    EXPECT_THROW(decode_l2_diff("not base64!"), std::invalid_argument);
}

TEST(L2DiffDecoder, RejectsInvalidDeflateData) {
    // "AAAA" is valid base64 (decodes to 3 zero bytes) but not valid raw deflate
    EXPECT_THROW(decode_l2_diff("AAAA"), std::runtime_error);
}

TEST(L2DiffDecoder, RejectsNonJsonDeflatePayload) {
    // raw-deflate of "not json", base64-encoded: exercises the json::parse failure path
    EXPECT_THROW(decode_l2_diff("y8svUcgqzs8DAA=="), json::parse_error);
}
