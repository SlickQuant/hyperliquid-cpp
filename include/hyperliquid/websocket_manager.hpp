#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <slick/dynamic_buffer.hpp>

namespace slick::net {
    template<typename BufferT> class Websocket;
}

namespace hyperliquid {

using Websocket = slick::net::Websocket<slick::dynamic_buffer<slick::stream_buffer_multiplexer::producer_buffer>>;

// Manages a single WebSocket connection to a Hyperliquid endpoint.
// Supports multiple subscriptions routing by channel identifier.
// Sends periodic pings to keep the connection alive.
class WebsocketManager {
public:
    static constexpr uint32_t INVALID_PRODUCER_ID = std::numeric_limits<uint32_t>::max();

    explicit WebsocketManager(
        std::string_view http_base_url,
        uint32_t mux_record_size = 1u << 18,           // 256K message records
        const char* mux_shm_name = nullptr,            // default not using shared memory
        uint32_t read_buffer_size = 1u << 24,          // 16 MB reading buffer
        uint32_t read_control_size = 1u << 16,         // 64K control size
        const char* read_buffer_shm_name = nullptr,    // default not using shared memory
        uint32_t write_buffer_size = 1u << 20,         // 1 MB writing buffer
        bool user_thread_dispatch = false              // if true, callbacks fire on dispatch() caller thread
    );
    explicit WebsocketManager(
        std::string_view http_base_url,
        slick::stream_buffer_multiplexer &mux,
        uint32_t read_buffer_size = 1u << 24,          // 16 MB reading buffer
        uint32_t read_control_size = 1u << 16,         // 64K control size
        const char* read_buffer_shm_name = nullptr,    // default not using shared memory
        uint32_t write_buffer_size = 1u << 20,         // 1 MB writing buffer
        bool user_thread_dispatch = false              // if true, callbacks fire on dispatch() caller thread
    );
    ~WebsocketManager();

    WebsocketManager(const WebsocketManager&) = delete;
    WebsocketManager& operator=(const WebsocketManager&) = delete;
    WebsocketManager(WebsocketManager&&) = delete;
    WebsocketManager& operator=(WebsocketManager&&) = delete;

    // Subscribe to a channel. Returns a subscription_id for later unsubscribe.
    // `subscription` follows the Python SDK format: {"type": "l2Book", "coin": "ETH"}, etc.
    // Throws std::runtime_error if the channel does not support multiple simultaneous subscriptions.
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

    slick::stream_buffer_multiplexer& mux() noexcept {
        return mux_;
    }

    uint32_t producer_id() const noexcept {
        return producer_id_;
    }

    bool user_thread_dispatch() const noexcept {
        return user_thread_dispatch_;
    }

    // Scan at most max_count queued records and invoke callbacks for records
    // belonging to this WebsocketManager on the calling thread. Only meaningful
    // when constructed with user_thread_dispatch=true; calling it in
    // service-thread mode is a no-op.
    // Returns the number of messages dispatched to this manager's callbacks.
    size_t dispatch(std::size_t max_count);

    // Dispatch a message to their registered callbacks on the calling thread
    // Only meaningful when the WebsocketManager was constructed with user_thread_dispatch=true
    // calling it in service-thread mode is a no-op.
    // Return true if message dispatched
    bool dispatch(uint32_t producer_id, const char* data, std::size_t length);

private:
    struct SubscriptionState;
    using SubscriptionStatePtr = std::shared_ptr<SubscriptionState>;

    void init(
        uint32_t read_buffer_size,
        uint32_t read_control_size,
        const char* read_buffer_shm_name,
        uint32_t write_buffer_size
    );
    void dispatch_message(const char* data, std::size_t len);
    void on_connected();
    void on_disconnected();
    void on_message(const char* data, std::size_t len);
    void on_error(std::string err);
    void resubscribe_all();  // re-sends all subscribe messages; called from on_connected()
    void send_subscribe_if_active(const SubscriptionStatePtr& state);
    void activate_subscription(const SubscriptionStatePtr& state);
    void deactivate_subscription(const SubscriptionStatePtr& state,
                                 const nlohmann::json& subscription);
    SubscriptionStatePtr subscription_state_for(
        const std::string& identifier,
        const nlohmann::json& subscription);

    void ping_loop();        // runs in ping_thread_; also manages reconnection

    std::string ws_url_;
    std::unique_ptr<Websocket> ws_;

    std::unique_ptr<slick::stream_buffer_multiplexer> owning_mux_;
    slick::stream_buffer_multiplexer &mux_;

    enum class SubscriptionPhase : std::uint8_t {
        Inactive,
        Active,
        Unsubscribing,
    };

    // Per-identifier state is shared by all callbacks for that subscription.
    // It serializes reconnect replay and final unsubscribe with atomics only:
    // final unsubscribe waits for in-flight subscribe/replay sends before
    // sending the server-side unsubscribe.
    struct SubscriptionState {
        std::shared_ptr<const nlohmann::json> subscription;
        std::atomic<unsigned int> handler_count{0};
        std::atomic<unsigned int> send_in_flight{0};
        std::atomic<SubscriptionPhase> phase{SubscriptionPhase::Inactive};

        explicit SubscriptionState(const nlohmann::json& sub)
            : subscription(std::make_shared<const nlohmann::json>(sub)) {}
    };

    using SubStateMap = std::unordered_map<std::string, SubscriptionStatePtr>;

    // Handler snapshots are published with copy-on-write. Each snapshot holds
    // shared per-callback state so unsubscribe() can deactivate exactly one
    // callback and wait for only that callback's in-flight invocation to drain.
    struct Handler {
        int subscription_id;
        std::function<void(const nlohmann::json&)> callback;
        SubscriptionStatePtr subscription_state;
        std::atomic<bool> active{true};
        std::atomic<unsigned int> in_flight{0};

        Handler(int id,
                std::function<void(const nlohmann::json&)> cb,
                SubscriptionStatePtr state)
            : subscription_id(id)
            , callback(std::move(cb))
            , subscription_state(std::move(state)) {}
    };

    using HandlerPtr = std::shared_ptr<Handler>;
    using HandlerMap = std::unordered_map<std::string, std::vector<HandlerPtr>>;

    std::shared_ptr<const HandlerMap> handlers_;

    // Subscription states keyed by identifier, used to re-subscribe after reconnect.
    // States may remain as inactive tombstones so a new subscribe can safely
    // synchronize with a concurrent final unsubscribe for the same identifier.
    std::shared_ptr<const SubStateMap> sub_state_map_{std::make_shared<SubStateMap>()};

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{true};
    std::atomic<int>  next_id_{1};

    std::thread ping_thread_;
    uint32_t producer_id_ = INVALID_PRODUCER_ID;
    bool user_thread_dispatch_ = false;
    uint64_t consumer_cursor_ = 0;
};

} // namespace hyperliquid
