#pragma once

#include <nlohmann/json.hpp>

#include <string_view>

namespace hyperliquid {

// Decode the compact `data.c` payload from Hyperliquid's undocumented `l2`
// websocket channel into the JSON diff object used by the web app.
// Throws std::invalid_argument on malformed base64 or empty payload.
// Throws std::runtime_error on inflate failure or JSON parse failure.
nlohmann::json decode_l2_diff(std::string_view compressed_diff);

} // namespace hyperliquid
