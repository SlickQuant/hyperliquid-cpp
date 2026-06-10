#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace hyperliquid {

// Base HTTP client. Sends JSON POST requests to a Hyperliquid API endpoint.
class Api {
public:
    explicit Api(std::string_view base_url);
    virtual ~Api() = default;

    // POST `endpoint` (e.g. "/info" or "/exchange") with `payload` as JSON body.
    // Throws std::runtime_error on HTTP 4xx/5xx.
    virtual nlohmann::json post(std::string_view endpoint, const nlohmann::json& payload);

    struct HttpError : std::runtime_error {
        int status_code;
        HttpError(int code, const std::string& msg)
            : std::runtime_error(msg), status_code(code) {}
    };

protected:
    std::string base_url_;
};

} // namespace hyperliquid
