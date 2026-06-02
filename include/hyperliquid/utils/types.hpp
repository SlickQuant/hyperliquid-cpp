#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace hyperliquid {

// ── Time in force ────────────────────────────────────────────────────────────

enum class Tif {
    Alo, // Add liquidity only
    Ioc, // Immediate or cancel
    Gtc, // Good till cancel
};

inline std::string tif_to_string(Tif t) {
    switch (t) {
    case Tif::Alo: return "Alo";
    case Tif::Ioc: return "Ioc";
    case Tif::Gtc: return "Gtc";
    }
    return "Gtc";
}

// ── Order types ──────────────────────────────────────────────────────────────

struct LimitOrderType {
    Tif tif = Tif::Gtc;
};

struct TriggerOrderType {
    double trigger_px;
    bool   is_market;
    std::string tpsl; // "tp" or "sl"
};

using OrderType = std::variant<LimitOrderType, TriggerOrderType>;

// ── Client order ID (16-byte hex: "0x" + 32 hex chars) ──────────────────────

class Cloid {
public:
    explicit Cloid(std::string raw) : raw_(std::move(raw)) { validate(); }

    static Cloid from_int(uint64_t v) {
        char buf[35];
        std::snprintf(buf, sizeof(buf), "0x%032llx",
                      static_cast<unsigned long long>(v));
        return Cloid{buf};
    }

    static Cloid from_str(std::string_view s) {
        return Cloid{std::string(s)};
    }

    std::string_view to_raw() const noexcept { return raw_; }

    bool operator==(const Cloid& o) const noexcept { return raw_ == o.raw_; }

private:
    std::string raw_;

    void validate() const {
        if (raw_.size() < 2 || raw_[0] != '0' || (raw_[1] != 'x' && raw_[1] != 'X'))
            throw std::invalid_argument("Cloid must be a hex string starting with 0x");
        if (raw_.size() - 2 != 32)
            throw std::invalid_argument("Cloid must be exactly 16 bytes (32 hex chars after 0x)");
    }
};

// ── Order / cancel / modify request types ───────────────────────────────────

struct OrderRequest {
    std::string coin;
    bool        is_buy     = true;
    double      sz         = 0.0;
    double      limit_px   = 0.0;
    OrderType   order_type = LimitOrderType{Tif::Gtc};
    bool        reduce_only = false;
    std::optional<Cloid> cloid;
};

struct CancelRequest {
    std::string coin;
    int64_t     oid;
};

struct CancelByCloidRequest {
    std::string coin;
    Cloid       cloid;
};

struct ModifyRequest {
    int64_t      oid; // order ID to modify
    OrderRequest order;
};

// ── Builder fee ──────────────────────────────────────────────────────────────

// b = builder address, f = fee in tenths of basis points (e.g. 10 = 1 bp)
struct BuilderInfo {
    std::string b;
    int         f = 0;
};

// ── EIP-712 signature ────────────────────────────────────────────────────────

struct Signature {
    std::string r; // "0x" + 64 hex chars
    std::string s; // "0x" + 64 hex chars
    int         v; // 27 or 28
};

} // namespace hyperliquid
