#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace slick::net { class Websocket; }

namespace hyperliquid {

// Manages a single WebSocket connection to a Hyperliquid endpoint.
// Supports multiple subscriptions routing by channel identifier.
// Sends periodic pings to keep the connection alive.
class WebsocketManager {
public:
    explicit WebsocketManager(std::string_view http_base_url);
    ~WebsocketManager();

    WebsocketManager(const WebsocketManager&) = delete;
    WebsocketManager& operator=(const WebsocketManager&) = delete;

    // Subscribe to a channel. Returns a subscription_id for later unsubscribe.
    // `subscription` follows the Python SDK format: {"type": "l2Book", "coin": "ETH"}, etc.
    int subscribe(const nlohmann::json& subscription,
                  std::function<void(const nlohmann::json&)> callback);

    // Unsubscribe from a channel by the id returned by subscribe().
    void unsubscribe(const nlohmann::json& subscription, int subscription_id);

    // Build a channel identifier string from a subscription JSON.
    // e.g. l2Book+ETH → "l2Book:ETH", candle+BTC+5m → "candle:BTC,5m"
    static std::string to_identifier(const nlohmann::json& sub);

    // Build a channel identifier string from an inbound WebSocket message.
    static std::optional<std::string> message_to_identifier(const nlohmann::json& msg);

    // Convert "https://..." → "wss://.../ws"
    static std::string http_to_ws_url(std::string_view http_url);

private:

    void on_connected();
    void on_disconnected();
    void on_message(const char* data, std::size_t len);
    void on_error(std::string err);

    void ping_loop();        // runs in ping_thread_

    std::string ws_url_;
    std::unique_ptr<slick::net::Websocket> ws_;

    // Handler snapshots are published with copy-on-write. Each snapshot holds
    // shared per-callback state so unsubscribe() can deactivate exactly one
    // callback and wait for only that callback's in-flight invocation to drain.
    struct Handler {
        int subscription_id;
        std::function<void(const nlohmann::json&)> callback;
        std::atomic<bool> active{true};
        std::atomic<unsigned int> in_flight{0};

        Handler(int id, std::function<void(const nlohmann::json&)> cb)
            : subscription_id(id), callback(std::move(cb)) {}
    };

    using HandlerPtr = std::shared_ptr<Handler>;
    using HandlerMap = std::unordered_map<std::string, std::vector<HandlerPtr>>;

    std::atomic<std::shared_ptr<const HandlerMap>> handlers_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{true};
    std::atomic<int>  next_id_{1};

    std::thread ping_thread_;
};

} // namespace hyperliquid
