#pragma once

// Keccak-256 (Ethereum variant) — NOT FIPS SHA3-256.
// Domain separation byte 0x01, rate 136 bytes.
// OpenSSL's SHA3 is FIPS SHA3-256 (domain 0x06), which is incompatible with Ethereum.

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace hyperliquid {
namespace detail {

static constexpr uint64_t keccak_rc[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

static constexpr int keccak_rho[24] = {
     1,  3,  6, 10, 15, 21, 28, 36,
    45, 55,  2, 14, 27, 41, 56,  8,
    25, 43, 62, 18, 39, 61, 20, 44,
};

static constexpr int keccak_pi[24] = {
    10,  7, 11, 17, 18,  3,  5, 16,
     8, 21, 24,  4, 15, 23, 19, 13,
    12,  2, 20, 14, 22,  9,  6,  1,
};

inline void keccakf1600(uint64_t st[25]) noexcept {
    for (int round = 0; round < 24; ++round) {
        // theta
        uint64_t bc[5];
        for (int i = 0; i < 5; ++i)
            bc[i] = st[i] ^ st[i+5] ^ st[i+10] ^ st[i+15] ^ st[i+20];
        for (int i = 0; i < 5; ++i) {
            uint64_t t = bc[(i+4)%5] ^ std::rotl(bc[(i+1)%5], 1);
            for (int j = 0; j < 25; j += 5)
                st[j+i] ^= t;
        }
        // rho + pi
        uint64_t t = st[1];
        for (int i = 0; i < 24; ++i) {
            int j = keccak_pi[i];
            uint64_t tmp = st[j];
            st[j] = std::rotl(t, keccak_rho[i]);
            t = tmp;
        }
        // chi
        for (int j = 0; j < 25; j += 5) {
            uint64_t b[5];
            for (int i = 0; i < 5; ++i) b[i] = st[j+i];
            for (int i = 0; i < 5; ++i)
                st[j+i] ^= (~b[(i+1)%5]) & b[(i+2)%5];
        }
        // iota
        st[0] ^= keccak_rc[round];
    }
}

// Read 8 bytes as little-endian uint64
inline uint64_t le64(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
}

// Write uint64 as 8 little-endian bytes
inline void put_le64(uint8_t* p, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}

} // namespace detail

// Keccak-256: rate=136 bytes, domain=0x01 (Ethereum / pre-NIST Keccak)
inline std::array<uint8_t, 32> keccak256(const uint8_t* data, size_t len) noexcept {
    constexpr size_t RATE = 136; // 1088 bits

    uint64_t st[25] = {};

    // Absorb full blocks
    size_t offset = 0;
    while (offset + RATE <= len) {
        for (size_t i = 0; i < RATE / 8; ++i)
            st[i] ^= detail::le64(data + offset + i * 8);
        detail::keccakf1600(st);
        offset += RATE;
    }

    // Last (partial) block + padding
    uint8_t tail[RATE] = {};
    size_t rem = len - offset;
    std::memcpy(tail, data + offset, rem);
    tail[rem]        = 0x01; // Keccak domain (NOT 0x06 which is SHA3)
    tail[RATE - 1]  |= 0x80;

    for (size_t i = 0; i < RATE / 8; ++i)
        st[i] ^= detail::le64(tail + i * 8);
    detail::keccakf1600(st);

    // Squeeze first 32 bytes
    std::array<uint8_t, 32> out;
    for (size_t i = 0; i < 4; ++i)
        detail::put_le64(out.data() + i * 8, st[i]);
    return out;
}

inline std::array<uint8_t, 32> keccak256(std::string_view sv) noexcept {
    return keccak256(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

inline std::array<uint8_t, 32> keccak256(const std::vector<uint8_t>& v) noexcept {
    return keccak256(v.data(), v.size());
}

} // namespace hyperliquid
