#pragma once

#include <hyperliquid/utils/types.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace hyperliquid::signing {

// ── Float serialisation (mirrors Python's float_to_wire) ─────────────────────
// Formats to up to 8 decimal places, strips trailing zeros.
// "1100.0" → "1100", "0.2" → "0.2"
std::string float_to_wire(double x);

// USD amounts: multiply by 1e6 and round to integer
int64_t float_to_usd_int(double x);

// ── Wire-format helpers ───────────────────────────────────────────────────────

// Convert an OrderRequest to the compact wire JSON object {a,b,p,s,r,t,?c}.
// Uses nlohmann::ordered_json to preserve key insertion order for msgpack hashing.
nlohmann::ordered_json order_request_to_wire(const OrderRequest& req, int asset);

// Build the "order" action {type, orders, grouping, ?builder}.
nlohmann::ordered_json order_wires_to_action(
    std::vector<nlohmann::ordered_json> wires,
    const std::string& grouping = "na",
    std::optional<BuilderInfo> builder = {});

// ── Action hashing ────────────────────────────────────────────────────────────
// Reproduces Python's action_hash():
//   keccak256( msgpack(action) || nonce_be8 || vault_flag || [vault_bytes] || [expires_flag_be8] )
std::array<uint8_t, 32> action_hash(
    const nlohmann::ordered_json& action,
    const std::optional<std::string>& vault_address,
    int64_t nonce,
    std::optional<int64_t> expires_after = {});

// ── EIP-712 signing ───────────────────────────────────────────────────────────

// Sign an L1 action (order/cancel/leverage/margin/…) using the phantom-agent
// EIP-712 pattern with Exchange domain (chainId 1337).
Signature sign_l1_action(
    std::string_view private_key_hex,
    const nlohmann::ordered_json& action,
    const std::optional<std::string>& vault_address,
    int64_t nonce,
    bool is_mainnet,
    std::optional<int64_t> expires_after = {});

// Sign a user-signed action (usdSend, spotSend, withdraw, …) using EIP-712
// with domain "HyperliquidSignTransaction" and chainId 0x66eee.
// `action` is modified in-place to add signatureChainId and hyperliquidChain.
// `payload_types` is a list of {field_name, solidity_type} pairs.
Signature sign_user_signed_action(
    std::string_view private_key_hex,
    nlohmann::ordered_json& action,
    const std::vector<std::pair<std::string, std::string>>& payload_types,
    std::string_view primary_type,
    bool is_mainnet);

// ── Key utilities ─────────────────────────────────────────────────────────────

// Derive Ethereum address (checksummed lower-case "0x...") from secp256k1 private key hex.
std::string private_key_to_address(std::string_view private_key_hex);

// Current timestamp in milliseconds since UNIX epoch.
int64_t get_timestamp_ms();

// Hex helpers
std::string bytes_to_hex(const uint8_t* data, size_t len, bool prefix = true);
std::vector<uint8_t> hex_to_bytes(std::string_view hex);

} // namespace hyperliquid::signing
