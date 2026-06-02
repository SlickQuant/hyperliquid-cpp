#include <hyperliquid/api.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <slick/net/http.hpp>

namespace hyperliquid {

Api::Api(std::string_view base_url) : base_url_(base_url) {
    // Strip trailing slash
    while (!base_url_.empty() && base_url_.back() == '/')
        base_url_.pop_back();
}

nlohmann::json Api::post(std::string_view endpoint, const nlohmann::json& payload) {
    std::string url = base_url_ + std::string(endpoint);
    std::string body = payload.dump();

    auto resp = slick::net::Http::post(
        url, body,
        {{"Content-Type", "application/json"}});

    if (!resp.is_ok()) {
        throw HttpError(
            static_cast<int>(resp.result_code),
            "HTTP " + std::to_string(resp.result_code) +
            " " + resp.reason + ": " + resp.result_text);
    }

    try {
        return nlohmann::json::parse(resp.result_text);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(std::string("JSON parse error: ") + e.what() +
                                  " body: " + resp.result_text);
    }
}

} // namespace hyperliquid
