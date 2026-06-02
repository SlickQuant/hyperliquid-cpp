#include <gtest/gtest.h>
#include <hyperliquid/utils/keccak.hpp>

#include <array>
#include <cstdio>
#include <string>
#include <vector>

using hyperliquid::keccak256;

// Helper: convert a 32-byte array to a lowercase hex string (no "0x")
static std::string to_hex(const std::array<uint8_t, 32>& a) {
    std::string s;
    s.reserve(64);
    for (uint8_t b : a) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", b);
        s += buf;
    }
    return s;
}

// ── Known Ethereum (pre-NIST Keccak-256) vectors ─────────────────────────────
// These differ from FIPS SHA3-256. If these pass, the domain byte (0x01) is correct.

TEST(Keccak256, EmptyString) {
    EXPECT_EQ(to_hex(keccak256("")),
        "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
}

TEST(Keccak256, Abc) {
    EXPECT_EQ(to_hex(keccak256("abc")),
        "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
}

TEST(Keccak256, TransferEventSignature) {
    // This is the well-known ERC-20 Transfer topic
    EXPECT_EQ(to_hex(keccak256("Transfer(address,address,uint256)")),
        "ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef");
}

TEST(Keccak256, ApprovalEventSignature) {
    // ERC-20 Approval topic
    EXPECT_EQ(to_hex(keccak256("Approval(address,address,uint256)")),
        "8c5be1e5ebec7d5bd14f71427d1e84f3dd0314c0f7b2291e5b200ac8c7c3b925");
}

// ── Overload consistency ──────────────────────────────────────────────────────

TEST(Keccak256, StringViewMatchesRawPtr) {
    const std::string input = "Hello Hyperliquid";
    auto h_raw = keccak256(
        reinterpret_cast<const uint8_t*>(input.data()), input.size());
    auto h_sv  = keccak256(std::string_view(input));
    EXPECT_EQ(h_raw, h_sv);
}

TEST(Keccak256, VectorMatchesRawPtr) {
    const std::string input = "secp256k1 EIP-712";
    auto h_raw = keccak256(
        reinterpret_cast<const uint8_t*>(input.data()), input.size());
    auto h_vec = keccak256(std::vector<uint8_t>(input.begin(), input.end()));
    EXPECT_EQ(h_raw, h_vec);
}

TEST(Keccak256, AllThreeOverloadsAgree) {
    const std::string input = "action_hash_test_string";
    auto h1 = keccak256(
        reinterpret_cast<const uint8_t*>(input.data()), input.size());
    auto h2 = keccak256(std::string_view(input));
    auto h3 = keccak256(std::vector<uint8_t>(input.begin(), input.end()));
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1, h3);
}

// ── Block boundary conditions ─────────────────────────────────────────────────
// Keccak rate = 136 bytes. Test around the single-block boundary.

TEST(Keccak256, Exactly135Bytes) {
    // 135 bytes: last data byte and padding share the same partial block
    std::string s(135, 'x');
    auto h1 = keccak256(std::string_view(s));
    auto h2 = keccak256(std::vector<uint8_t>(s.begin(), s.end()));
    EXPECT_EQ(h1, h2);
    EXPECT_NE(to_hex(h1), std::string(64, '0'));
}

TEST(Keccak256, Exactly136Bytes) {
    // 136 bytes: exactly one full block, next block is only padding
    std::string s(136, 'x');
    auto h1 = keccak256(std::string_view(s));
    auto h2 = keccak256(std::vector<uint8_t>(s.begin(), s.end()));
    EXPECT_EQ(h1, h2);
    EXPECT_NE(to_hex(h1), std::string(64, '0'));
}

TEST(Keccak256, Exactly137Bytes) {
    // 137 bytes: one full block plus one-byte partial block
    std::string s(137, 'x');
    auto h1 = keccak256(std::string_view(s));
    auto h2 = keccak256(std::vector<uint8_t>(s.begin(), s.end()));
    EXPECT_EQ(h1, h2);
    EXPECT_NE(to_hex(h1), std::string(64, '0'));
}

TEST(Keccak256, BoundaryHashesDiffer) {
    // 135, 136, 137 byte inputs of the same char must all produce distinct hashes
    std::array<uint8_t, 32> h135 = keccak256(std::string(135, 'x'));
    std::array<uint8_t, 32> h136 = keccak256(std::string(136, 'x'));
    std::array<uint8_t, 32> h137 = keccak256(std::string(137, 'x'));
    EXPECT_NE(h135, h136);
    EXPECT_NE(h136, h137);
    EXPECT_NE(h135, h137);
}

TEST(Keccak256, MultiBlock200Bytes) {
    // 200 bytes: requires two block absorptions
    std::string s(200, 'a');
    auto h1 = keccak256(
        reinterpret_cast<const uint8_t*>(s.data()), s.size());
    auto h2 = keccak256(std::string_view(s));
    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, keccak256(std::string_view("a")));
}

// ── Sensitivity and correctness ───────────────────────────────────────────────

TEST(Keccak256, SingleBitChange) {
    // Changing one character must change the hash completely
    std::string a = "hello";
    std::string b = "hellp";
    EXPECT_NE(keccak256(std::string_view(a)), keccak256(std::string_view(b)));
}

TEST(Keccak256, OrderMatters) {
    std::vector<uint8_t> fwd = {0x01, 0x02, 0x03};
    std::vector<uint8_t> rev = {0x03, 0x02, 0x01};
    EXPECT_NE(keccak256(fwd), keccak256(rev));
}

TEST(Keccak256, BinaryDataWithNullBytes) {
    std::vector<uint8_t> data = {0x00, 0x00, 0x01, 0xff};
    auto h = keccak256(data);
    EXPECT_NE(to_hex(h), std::string(64, '0'));
}

TEST(Keccak256, AllZeroBytes) {
    // 32 zero bytes: a common input in EIP-712
    std::vector<uint8_t> zeros(32, 0x00);
    EXPECT_EQ(to_hex(keccak256(zeros)),
        "290decd9548b62a8d60345a988386fc84ba6bc95484008f6362f93160ef3e563");
}

TEST(Keccak256, Deterministic) {
    auto h1 = keccak256("determinism_test");
    auto h2 = keccak256("determinism_test");
    EXPECT_EQ(h1, h2);
}
