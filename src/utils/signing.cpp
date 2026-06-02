#include <hyperliquid/utils/signing.hpp>
#include <hyperliquid/utils/keccak.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

// OpenSSL secp256k1 ECDSA
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>

namespace hyperliquid::signing {

// ── Hex utilities ─────────────────────────────────────────────────────────────

std::string bytes_to_hex(const uint8_t* data, size_t len, bool prefix) {
    std::string out;
    out.reserve((prefix ? 2 : 0) + len * 2);
    if (prefix) out += "0x";
    for (size_t i = 0; i < len; ++i) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", data[i]);
        out += buf;
    }
    return out;
}

std::vector<uint8_t> hex_to_bytes(std::string_view hex) {
    if (hex.starts_with("0x") || hex.starts_with("0X"))
        hex.remove_prefix(2);
    if (hex.size() % 2 != 0)
        throw std::invalid_argument("hex_to_bytes: odd length hex string");
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned v = 0;
        std::sscanf(hex.data() + i * 2, "%02x", &v);
        out[i] = static_cast<uint8_t>(v);
    }
    return out;
}

// ── Float serialisation ───────────────────────────────────────────────────────

std::string float_to_wire(double x) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.8f", x);
    std::string s = buf;

    // Remove trailing zeros after decimal point
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        auto last = s.find_last_not_of('0');
        if (last == dot) {
            s = s.substr(0, dot); // e.g. "1100.00000000" → "1100"
        } else if (last != std::string::npos) {
            s = s.substr(0, last + 1); // e.g. "0.20000000" → "0.2"
        }
    }

    // Normalise negative zero
    if (s == "-0") s = "0";
    return s;
}

int64_t float_to_usd_int(double x) {
    double scaled = x * 1e6;
    return static_cast<int64_t>(std::round(scaled));
}

// ── Wire format helpers ───────────────────────────────────────────────────────

nlohmann::ordered_json order_request_to_wire(const OrderRequest& req, int asset) {
    nlohmann::ordered_json wire;
    wire["a"] = asset;
    wire["b"] = req.is_buy;
    wire["p"] = float_to_wire(req.limit_px);
    wire["s"] = float_to_wire(req.sz);
    wire["r"] = req.reduce_only;

    if (std::holds_alternative<LimitOrderType>(req.order_type)) {
        const auto& lt = std::get<LimitOrderType>(req.order_type);
        nlohmann::ordered_json t;
        nlohmann::ordered_json lim;
        lim["tif"] = tif_to_string(lt.tif);
        t["limit"] = lim;
        wire["t"] = t;
    } else {
        const auto& tt = std::get<TriggerOrderType>(req.order_type);
        nlohmann::ordered_json t;
        nlohmann::ordered_json trig;
        trig["triggerPx"] = float_to_wire(tt.trigger_px);
        trig["isMarket"]  = tt.is_market;
        trig["tpsl"]      = tt.tpsl;
        t["trigger"] = trig;
        wire["t"] = t;
    }

    if (req.cloid)
        wire["c"] = std::string(req.cloid->to_raw());

    return wire;
}

nlohmann::ordered_json order_wires_to_action(
    std::vector<nlohmann::ordered_json> wires,
    const std::string& grouping,
    std::optional<BuilderInfo> builder)
{
    nlohmann::ordered_json action;
    action["type"]     = "order";
    action["orders"]   = wires;
    action["grouping"] = grouping;
    if (builder) {
        nlohmann::ordered_json b;
        b["b"] = builder->b;
        b["f"] = builder->f;
        action["builder"] = b;
    }
    return action;
}

// ── Action hash ───────────────────────────────────────────────────────────────

std::array<uint8_t, 32> action_hash(
    const nlohmann::ordered_json& action,
    const std::optional<std::string>& vault_address,
    int64_t nonce,
    std::optional<int64_t> expires_after)
{
    // msgpack(action)
    std::vector<uint8_t> data = nlohmann::ordered_json::to_msgpack(action);

    // nonce as 8-byte big-endian
    for (int i = 7; i >= 0; --i)
        data.push_back(static_cast<uint8_t>((nonce >> (i * 8)) & 0xFF));

    // vault address flag
    if (!vault_address) {
        data.push_back(0x00);
    } else {
        data.push_back(0x01);
        auto addr_bytes = hex_to_bytes(*vault_address);
        if (addr_bytes.size() != 20)
            throw std::invalid_argument("vault_address must be 20 bytes");
        data.insert(data.end(), addr_bytes.begin(), addr_bytes.end());
    }

    // optional expires_after (flag 0x00 then 8 bytes big-endian)
    if (expires_after) {
        data.push_back(0x00);
        int64_t ea = *expires_after;
        for (int i = 7; i >= 0; --i)
            data.push_back(static_cast<uint8_t>((ea >> (i * 8)) & 0xFF));
    }

    return keccak256(data);
}

// ── EIP-712 encoding ──────────────────────────────────────────────────────────

namespace {

// Encode a value according to its Solidity type for EIP-712 encodeData.
// All results are exactly 32 bytes.
std::array<uint8_t, 32> encode_eip712_field(
    std::string_view solidity_type, const nlohmann::json& value)
{
    std::array<uint8_t, 32> out{};

    if (solidity_type == "string") {
        std::string s = value.get<std::string>();
        out = keccak256(s);
    } else if (solidity_type == "bytes32") {
        auto hx = value.get<std::string>();
        auto b = hex_to_bytes(hx);
        size_t n = std::min(b.size(), size_t{32});
        std::copy_n(b.begin(), n, out.begin());
    } else if (solidity_type == "address") {
        auto hx = value.get<std::string>();
        auto b = hex_to_bytes(hx);
        size_t n = std::min(b.size(), size_t{20});
        std::copy_n(b.begin(), n, out.begin() + (32 - n));
    } else if (solidity_type == "bool") {
        out[31] = value.get<bool>() ? 1 : 0;
    } else if (solidity_type == "uint256" || solidity_type == "uint64") {
        uint64_t v = 0;
        if (value.is_string()) {
            // hex string e.g. "0x66eee"
            std::string s = value.get<std::string>();
            if (s.starts_with("0x") || s.starts_with("0X"))
                s = s.substr(2);
            v = std::stoull(s, nullptr, 16);
        } else {
            v = value.get<uint64_t>();
        }
        for (int i = 7; i >= 0; --i)
            out[24 + (7 - i)] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    }

    return out;
}

// Build the EIP-712 type string, e.g.:
//   "HyperliquidTransaction:UsdSend(string hyperliquidChain,string destination,...)"
std::string make_type_string(
    std::string_view primary_type,
    const std::vector<std::pair<std::string, std::string>>& fields)
{
    std::string s(primary_type);
    s += '(';
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) s += ',';
        s += fields[i].second; // type
        s += ' ';
        s += fields[i].first;  // name
    }
    s += ')';
    return s;
}

// EIP712Domain type string (always the same 4 fields in this order)
static constexpr std::string_view DOMAIN_TYPE_STR =
    "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";

// Compute the domain separator for a given domain.
std::array<uint8_t, 32> compute_domain_separator(
    std::string_view name, std::string_view version,
    uint64_t chain_id, std::string_view verifying_contract)
{
    std::array<uint8_t, 32> type_hash = keccak256(DOMAIN_TYPE_STR);

    std::vector<uint8_t> buf;
    buf.insert(buf.end(), type_hash.begin(), type_hash.end());

    // name
    auto nh = keccak256(name);
    buf.insert(buf.end(), nh.begin(), nh.end());

    // version
    auto vh = keccak256(version);
    buf.insert(buf.end(), vh.begin(), vh.end());

    // chainId (uint256, big-endian 32 bytes)
    std::array<uint8_t, 32> chain_bytes{};
    for (int i = 7; i >= 0; --i)
        chain_bytes[24 + (7 - i)] = static_cast<uint8_t>((chain_id >> (i * 8)) & 0xFF);
    buf.insert(buf.end(), chain_bytes.begin(), chain_bytes.end());

    // verifyingContract (address, 32 bytes right-aligned)
    auto addr_bytes = hex_to_bytes(verifying_contract);
    std::array<uint8_t, 32> addr32{};
    size_t n = std::min(addr_bytes.size(), size_t{20});
    std::copy_n(addr_bytes.begin(), n, addr32.begin() + (32 - n));
    buf.insert(buf.end(), addr32.begin(), addr32.end());

    return keccak256(buf);
}

// Compute hashStruct for a typed message.
std::array<uint8_t, 32> hash_struct(
    std::string_view primary_type,
    const std::vector<std::pair<std::string, std::string>>& field_types,
    const nlohmann::json& message)
{
    std::string type_str = make_type_string(primary_type, field_types);
    std::array<uint8_t, 32> type_hash = keccak256(type_str);

    std::vector<uint8_t> buf;
    buf.insert(buf.end(), type_hash.begin(), type_hash.end());

    for (const auto& [name, type] : field_types) {
        auto encoded = encode_eip712_field(type, message.at(name));
        buf.insert(buf.end(), encoded.begin(), encoded.end());
    }

    return keccak256(buf);
}

// Final EIP-712 hash: keccak256("\x19\x01" || domain_sep || struct_hash)
std::array<uint8_t, 32> eip712_hash(
    const std::array<uint8_t, 32>& domain_sep,
    const std::array<uint8_t, 32>& struct_hash)
{
    std::vector<uint8_t> buf;
    buf.push_back(0x19);
    buf.push_back(0x01);
    buf.insert(buf.end(), domain_sep.begin(), domain_sep.end());
    buf.insert(buf.end(), struct_hash.begin(), struct_hash.end());
    return keccak256(buf);
}

} // anonymous namespace

// ── secp256k1 ECDSA signing (OpenSSL) ─────────────────────────────────────────

namespace {

// Parse private key hex → EC_KEY with secp256k1 curve (also sets public key)
EC_KEY* load_ec_key(std::string_view priv_hex) {
    std::string hex(priv_hex);
    if (hex.starts_with("0x") || hex.starts_with("0X"))
        hex = hex.substr(2);

    BIGNUM* priv_bn = nullptr;
    if (!BN_hex2bn(&priv_bn, hex.c_str()))
        throw std::runtime_error("Failed to parse private key hex");

    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!key) { BN_free(priv_bn); throw std::runtime_error("EC_KEY_new failed"); }

    if (EC_KEY_set_private_key(key, priv_bn) != 1) {
        BN_free(priv_bn); EC_KEY_free(key);
        throw std::runtime_error("EC_KEY_set_private_key failed");
    }

    // Derive public key
    const EC_GROUP* group = EC_KEY_get0_group(key);
    EC_POINT*       pub   = EC_POINT_new(group);
    BN_CTX*         ctx   = BN_CTX_new();

    if (!EC_POINT_mul(group, pub, priv_bn, nullptr, nullptr, ctx)) {
        BN_free(priv_bn); EC_POINT_free(pub); BN_CTX_free(ctx); EC_KEY_free(key);
        throw std::runtime_error("Public key derivation failed");
    }

    EC_KEY_set_public_key(key, pub);
    EC_POINT_free(pub);
    BN_CTX_free(ctx);
    BN_free(priv_bn);
    return key;
}

// Attempt to recover public key from (r, s, hash) with the given recid.
// Returns nullptr if recovery fails or point is invalid.
EC_POINT* recover_pub_key(
    const EC_GROUP* group, const BIGNUM* r, const BIGNUM* s,
    const BIGNUM* z, int recid, BN_CTX* ctx)
{
    BIGNUM* order = BN_new();
    EC_GROUP_get_order(group, order, ctx);

    // x-coordinate of R
    BIGNUM* x = BN_dup(r);
    if (recid >= 2) BN_add(x, x, order); // rare for secp256k1

    // Reconstruct R from x with parity (recid & 1)
    EC_POINT* R = EC_POINT_new(group);
    if (!EC_POINT_set_compressed_coordinates(group, R, x, recid & 1, ctx)) {
        BN_free(x); BN_free(order); EC_POINT_free(R);
        return nullptr;
    }

    // r^(-1) mod n
    BIGNUM* r_inv = BN_mod_inverse(nullptr, r, order, ctx);

    // u1 = (-z * r^(-1)) mod n
    BIGNUM* z_neg = BN_new();
    BN_mod_sub(z_neg, order, z, order, ctx);
    BIGNUM* u1 = BN_new();
    BN_mod_mul(u1, z_neg, r_inv, order, ctx);

    // u2 = (s * r^(-1)) mod n
    BIGNUM* u2 = BN_new();
    BN_mod_mul(u2, s, r_inv, order, ctx);

    // Q = u1*G + u2*R
    EC_POINT* Q = EC_POINT_new(group);
    EC_POINT_mul(group, Q, u1, R, u2, ctx);

    BN_free(order); BN_free(x); BN_free(r_inv); BN_free(z_neg);
    BN_free(u1); BN_free(u2); EC_POINT_free(R);
    return Q; // caller must EC_POINT_free
}

// Sign a 32-byte hash with the given EC_KEY, returning {r, s, v=recid+27}.
Signature sign_hash_with_key(EC_KEY* key, const std::array<uint8_t, 32>& hash) {
    ECDSA_SIG* sig = ECDSA_do_sign(hash.data(), 32, key);
    if (!sig) throw std::runtime_error("ECDSA_do_sign failed");

    const BIGNUM* r_bn = nullptr;
    const BIGNUM* s_bn = nullptr;
    ECDSA_SIG_get0(sig, &r_bn, &s_bn);

    // Find recovery ID
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM* z    = BN_bin2bn(hash.data(), 32, nullptr);
    const EC_GROUP* group = EC_KEY_get0_group(key);
    const EC_POINT* expected_pub = EC_KEY_get0_public_key(key);

    int recid = -1;
    for (int i = 0; i <= 1; ++i) {
        EC_POINT* Q = recover_pub_key(group, r_bn, s_bn, z, i, ctx);
        if (Q) {
            if (EC_POINT_cmp(group, Q, expected_pub, ctx) == 0)
                recid = i;
            EC_POINT_free(Q);
            if (recid >= 0) break;
        }
    }
    BN_free(z);
    BN_CTX_free(ctx);

    if (recid < 0) { ECDSA_SIG_free(sig); throw std::runtime_error("Could not find recovery ID"); }

    // Format r and s as "0x" + 64 hex chars (32 bytes, zero-padded)
    auto bn_to_hex32 = [](const BIGNUM* bn) -> std::string {
        char* hex = BN_bn2hex(bn);
        std::string s(hex);
        OPENSSL_free(hex);
        while (s.size() < 64) s = "0" + s;
        for (char& c : s) c = std::tolower(static_cast<unsigned char>(c));
        return "0x" + s;
    };

    Signature result{bn_to_hex32(r_bn), bn_to_hex32(s_bn), recid + 27};
    ECDSA_SIG_free(sig);
    return result;
}

} // anonymous namespace

// ── Public signing API ────────────────────────────────────────────────────────

Signature sign_l1_action(
    std::string_view private_key_hex,
    const nlohmann::ordered_json& action,
    const std::optional<std::string>& vault_address,
    int64_t nonce,
    bool is_mainnet,
    std::optional<int64_t> expires_after)
{
    auto hash = action_hash(action, vault_address, nonce, expires_after);

    // Phantom agent
    std::string source = is_mainnet ? "a" : "b";
    std::string conn_id_hex = bytes_to_hex(hash.data(), 32); // "0x" + 64 hex

    // EIP-712 with Exchange domain (chainId 1337)
    auto domain_sep = compute_domain_separator(
        "Exchange", "1", 1337,
        "0x0000000000000000000000000000000000000000");

    // Agent struct: {source: string, connectionId: bytes32}
    std::vector<std::pair<std::string, std::string>> agent_fields = {
        {"source",       "string"},
        {"connectionId", "bytes32"},
    };
    nlohmann::json agent_msg;
    agent_msg["source"]       = source;
    agent_msg["connectionId"] = conn_id_hex;

    auto struct_h = hash_struct("Agent", agent_fields, agent_msg);
    auto final_h  = eip712_hash(domain_sep, struct_h);

    EC_KEY* key = load_ec_key(private_key_hex);
    auto sig = sign_hash_with_key(key, final_h);
    EC_KEY_free(key);
    return sig;
}

Signature sign_user_signed_action(
    std::string_view private_key_hex,
    nlohmann::ordered_json& action,
    const std::vector<std::pair<std::string, std::string>>& payload_types,
    std::string_view primary_type,
    bool is_mainnet)
{
    action["signatureChainId"] = "0x66eee";
    action["hyperliquidChain"] = is_mainnet ? "Mainnet" : "Testnet";

    uint64_t chain_id = 0x66eee;

    auto domain_sep = compute_domain_separator(
        "HyperliquidSignTransaction", "1", chain_id,
        "0x0000000000000000000000000000000000000000");

    auto struct_h = hash_struct(primary_type, payload_types, action);
    auto final_h  = eip712_hash(domain_sep, struct_h);

    EC_KEY* key = load_ec_key(private_key_hex);
    auto sig = sign_hash_with_key(key, final_h);
    EC_KEY_free(key);
    return sig;
}

// ── Key utilities ─────────────────────────────────────────────────────────────

std::string private_key_to_address(std::string_view private_key_hex) {
    EC_KEY* key = load_ec_key(private_key_hex);
    const EC_GROUP* group = EC_KEY_get0_group(key);
    const EC_POINT* pub   = EC_KEY_get0_public_key(key);
    BN_CTX* ctx = BN_CTX_new();

    // Uncompressed public key: 0x04 || x (32) || y (32) = 65 bytes
    uint8_t pub_bytes[65];
    size_t n = EC_POINT_point2oct(group, pub, POINT_CONVERSION_UNCOMPRESSED,
                                   pub_bytes, sizeof(pub_bytes), ctx);
    BN_CTX_free(ctx);
    EC_KEY_free(key);

    if (n != 65) throw std::runtime_error("Unexpected public key length");

    // Keccak-256 of x||y (skip the 0x04 prefix byte)
    auto h = keccak256(pub_bytes + 1, 64);

    // Address = last 20 bytes
    return bytes_to_hex(h.data() + 12, 20);
}

int64_t get_timestamp_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

} // namespace hyperliquid::signing
