#include <hyperliquid/websocket_manager.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <slick/net/websocket.hpp>
#include <slick/net/logging.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

namespace {

thread_local const void* dispatching_handler = nullptr;

void wait_for_zero(std::atomic<unsigned int>& counter) {
    unsigned int in_flight = counter.load(std::memory_order_acquire);
    while (in_flight != 0) {
        counter.wait(in_flight, std::memory_order_relaxed);
        in_flight = counter.load(std::memory_order_acquire);
    }
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool is_single_subscriber_channel(std::string_view identifier) {
    return identifier == "userEvents" || identifier == "orderUpdates";
}

} // namespace

namespace hyperliquid {

// URL helpers

std::string WebsocketManager::http_to_ws_url(std::string_view http_url) {
    std::string ws_url(http_url);
    if (ws_url.starts_with("https://")) {
        ws_url.replace(0, 8, "wss://");
    } else if (ws_url.starts_with("http://")) {
        ws_url.replace(0, 7, "ws://");
    }
    while (!ws_url.empty() && ws_url.back() == '/')
        ws_url.pop_back();
    ws_url += "/ws";
    return ws_url;
}

// Identifier mapping

std::string WebsocketManager::to_identifier(const nlohmann::json& sub) {
    const std::string type = sub.at("type").get<std::string>();

    if (type == "allMids")
        return "allMids";
    if (type == "l2Book")
        return "l2Book:" + sub.at("coin").get<std::string>();
    if (type == "bbo")
        return "bbo:" + sub.at("coin").get<std::string>();
    if (type == "trades")
        return "trades:" + sub.at("coin").get<std::string>();
    if (type == "candle")
        return "candle:" + sub.at("coin").get<std::string>() + "," +
               sub.at("interval").get<std::string>();
    if (type == "userEvents")
        return "userEvents";
    if (type == "userFills")
        return "userFills:" + lowercase_copy(sub.at("user").get<std::string>());
    if (type == "orderUpdates")
        return "orderUpdates";
    if (type == "userFundings")
        return "userFundings:" + lowercase_copy(sub.at("user").get<std::string>());
    if (type == "userNonFundingLedgerUpdates")
        return "userNonFundingLedgerUpdates:" + lowercase_copy(sub.at("user").get<std::string>());
    if (type == "webData2")
        return "webData2:" + lowercase_copy(sub.at("user").get<std::string>());
    if (type == "activeAssetCtx")
        return "activeAssetCtx:" + sub.at("coin").get<std::string>();
    if (type == "activeAssetData")
        return "activeAssetData:" + sub.at("coin").get<std::string>() + "," +
               lowercase_copy(sub.at("user").get<std::string>());
    return type;
}

std::optional<std::string> WebsocketManager::message_to_identifier(const nlohmann::json& msg) {
    const std::string channel = msg.value("channel", "");
    if (channel.empty())
        return std::nullopt;
    if (channel == "pong")
        return "pong";
    if (channel == "allMids")
        return "allMids";
    if (channel == "l2Book")
        return "l2Book:" + msg.at("data").at("coin").get<std::string>();
    if (channel == "trades") {
        const auto& trades = msg.at("data");
        if (!trades.is_array() || trades.empty())
            return std::nullopt;
        return "trades:" + trades.at(0).at("coin").get<std::string>();
    }
    if (channel == "user")
        return "userEvents";
    if (channel == "userFills")
        return "userFills:" + lowercase_copy(msg.at("data").at("user").get<std::string>());
    if (channel == "candle")
        return "candle:" + msg.at("data").at("s").get<std::string>() + "," +
               msg.at("data").at("i").get<std::string>();
    if (channel == "orderUpdates")
        return "orderUpdates";
    if (channel == "userFundings")
        return "userFundings:" + lowercase_copy(msg.at("data").at("user").get<std::string>());
    if (channel == "userNonFundingLedgerUpdates")
        return "userNonFundingLedgerUpdates:" + lowercase_copy(msg.at("data").at("user").get<std::string>());
    if (channel == "webData2")
        return "webData2:" + lowercase_copy(msg.at("data").at("user").get<std::string>());
    if (channel == "bbo")
        return "bbo:" + msg.at("data").at("coin").get<std::string>();
    if (channel == "activeAssetCtx" || channel == "activeSpotAssetCtx")
        return "activeAssetCtx:" + msg.at("data").at("coin").get<std::string>();
    if (channel == "activeAssetData")
        return "activeAssetData:" + msg.at("data").at("coin").get<std::string>() + "," +
               lowercase_copy(msg.at("data").at("user").get<std::string>());
    return channel;
}

// Constructor / destructor

WebsocketManager::WebsocketManager(
    std::string_view http_base_url,
    uint32_t mux_record_size,
    const char* mux_shm_name,
    uint32_t read_buffer_size,
    uint32_t read_control_size,
    const char* read_buffer_shm_name,
    uint32_t write_buffer_size,
    bool user_thread_dispatch
)
    : ws_url_(http_to_ws_url(http_base_url))
    , owning_mux_(new slick::stream_buffer_multiplexer(mux_record_size, mux_shm_name))
    , mux_(*owning_mux_.get())
    , handlers_(std::make_shared<HandlerMap>())
    , user_thread_dispatch_(user_thread_dispatch)
{
    init(read_buffer_size, read_control_size, read_buffer_shm_name, write_buffer_size);
}

WebsocketManager::WebsocketManager(
    std::string_view http_base_url,
    slick::stream_buffer_multiplexer &mux,
    uint32_t read_buffer_size,
    uint32_t read_control_size,
    const char* read_buffer_shm_name,
    uint32_t write_buffer_size,
    bool user_thread_dispatch
)
    : ws_url_(http_to_ws_url(http_base_url))
    , mux_(mux)
    , handlers_(std::make_shared<HandlerMap>())
    , user_thread_dispatch_(user_thread_dispatch)
{
    init(read_buffer_size, read_control_size, read_buffer_shm_name, write_buffer_size);
}

WebsocketManager::~WebsocketManager() {
    running_.store(false, std::memory_order_release);
    if (ping_thread_.joinable()) {
        ping_thread_.join();
    }
    if (ws_) {
        ws_->detach();
        ws_->close();
    }
}

void WebsocketManager::init(
    uint32_t read_buffer_size,
    uint32_t read_control_size,
    const char* read_buffer_shm_name,
    uint32_t write_buffer_size
) {
    producer_id_ = mux_.producer_count();
    auto pd = mux_.add_producer(producer_id_, read_buffer_size, read_control_size, read_buffer_shm_name);
    if (user_thread_dispatch_)
        consumer_cursor_ = mux_.initial_reading_index();
    ws_ = std::make_unique<Websocket>(
        ws_url_,
        [this]() { on_connected(); },
        [this]() { on_disconnected(); },
        [this](const char* data, std::size_t len) { on_message(data, len); },
        [this](std::string&& err) { on_error(std::move(err)); },
        pd,
        write_buffer_size
    );

    ws_->open();
    ping_thread_ = std::thread([this]() { ping_loop(); });
}

// Connection callbacks

void WebsocketManager::on_connected() {
    connected_.store(true, std::memory_order_release);
    resubscribe_all();
}

void WebsocketManager::resubscribe_all() {
    const auto snap = std::atomic_load_explicit(&sub_state_map_, std::memory_order_acquire);
    for (const auto& [identifier, state] : *snap) {
        (void)identifier;
        send_subscribe_if_active(state);
    }
}

void WebsocketManager::send_subscribe_if_active(const SubscriptionStatePtr& state) {
    state->send_in_flight.fetch_add(1, std::memory_order_acq_rel);

    auto release_send = [&]() noexcept {
        if (state->send_in_flight.fetch_sub(1, std::memory_order_acq_rel) == 1)
            state->send_in_flight.notify_all();
    };

    try {
        if (state->handler_count.load(std::memory_order_acquire) != 0 &&
            state->phase.load(std::memory_order_acquire) == SubscriptionPhase::Active &&
            connected_.load(std::memory_order_acquire)) {
            const auto sub = std::atomic_load_explicit(&state->subscription,
                                                       std::memory_order_acquire);
            nlohmann::json msg{{"method", "subscribe"}, {"subscription", *sub}};
            const std::string serialized = msg.dump();
            ws_->send(serialized.c_str(), serialized.size());
        }
    } catch (...) {
        release_send();
        throw;
    }

    release_send();
}

void WebsocketManager::activate_subscription(const SubscriptionStatePtr& state) {
    bool should_send = false;

    for (;;) {
        if (state->handler_count.load(std::memory_order_acquire) == 0)
            return;

        auto phase = state->phase.load(std::memory_order_acquire);
        if (phase == SubscriptionPhase::Active)
            return;

        if (phase == SubscriptionPhase::Inactive) {
            if (state->phase.compare_exchange_weak(
                    phase,
                    SubscriptionPhase::Active,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                state->phase.notify_all();
                should_send = true;
                break;
            }
            continue;
        }

        state->phase.wait(phase, std::memory_order_relaxed);
    }

    if (should_send)
        send_subscribe_if_active(state);
}

void WebsocketManager::deactivate_subscription(const SubscriptionStatePtr& state,
                                               const nlohmann::json& subscription) {
    for (;;) {
        if (state->handler_count.load(std::memory_order_acquire) != 0)
            return;

        auto phase = state->phase.load(std::memory_order_acquire);
        if (phase == SubscriptionPhase::Inactive)
            return;

        if (phase == SubscriptionPhase::Active) {
            if (state->phase.compare_exchange_weak(
                    phase,
                    SubscriptionPhase::Unsubscribing,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
                break;
            continue;
        }

        state->phase.wait(phase, std::memory_order_relaxed);
    }

    wait_for_zero(state->send_in_flight);

    if (state->handler_count.load(std::memory_order_acquire) != 0) {
        state->phase.store(SubscriptionPhase::Active, std::memory_order_release);
        state->phase.notify_all();
        return;
    }

    try {
        if (connected_.load(std::memory_order_acquire)) {
            nlohmann::json msg{{"method", "unsubscribe"}, {"subscription", subscription}};
            const std::string serialized = msg.dump();
            ws_->send(serialized.c_str(), serialized.size());
        }
    } catch (...) {
        state->phase.store(SubscriptionPhase::Inactive, std::memory_order_release);
        state->phase.notify_all();
        throw;
    }

    state->phase.store(SubscriptionPhase::Inactive, std::memory_order_release);
    state->phase.notify_all();
}

WebsocketManager::SubscriptionStatePtr WebsocketManager::subscription_state_for(
    const std::string& identifier,
    const nlohmann::json& subscription)
{
    auto old = std::atomic_load_explicit(&sub_state_map_, std::memory_order_acquire);
    for (;;) {
        const auto it = old->find(identifier);
        if (it != old->end()) {
            std::atomic_store_explicit(
                &it->second->subscription,
                std::make_shared<const nlohmann::json>(subscription),
                std::memory_order_release);
            return it->second;
        }

        auto state = std::make_shared<SubscriptionState>(subscription);
        auto new_map = std::make_shared<SubStateMap>(*old);
        new_map->emplace(identifier, state);

        if (std::atomic_compare_exchange_weak_explicit(
                &sub_state_map_,
                &old,
                std::static_pointer_cast<const SubStateMap>(new_map),
                std::memory_order_release,
                std::memory_order_acquire))
            return state;
    }
}

void WebsocketManager::on_disconnected() {
    connected_.store(false, std::memory_order_release);
}

void WebsocketManager::on_message(const char* data, std::size_t len) {
    if (user_thread_dispatch_)
        return;
    dispatch_message(data, len);
}

void WebsocketManager::dispatch_message(const char* data, std::size_t len) {
    nlohmann::json msg;
    try {
        msg = nlohmann::json::parse(data, data + len);
    } catch (...) {
        return;
    }

    const auto identifier = message_to_identifier(msg);
    if (!identifier || *identifier == "pong")
        return;

    const auto handlers = std::atomic_load_explicit(&handlers_, std::memory_order_acquire);
    const auto it = handlers->find(*identifier);
    if (it == handlers->end())
        return;

    for (const auto& handler : it->second) {
        if (!handler->active.load(std::memory_order_acquire))
            continue;

        handler->in_flight.fetch_add(1, std::memory_order_acq_rel);
        if (!handler->active.load(std::memory_order_acquire)) {
            if (handler->in_flight.fetch_sub(1, std::memory_order_acq_rel) == 1)
                handler->in_flight.notify_all();
            continue;
        }

        const void* previous = dispatching_handler;
        dispatching_handler = handler.get();
        try {
            handler->callback(msg);
        } catch (...) {
            dispatching_handler = previous;
            if (handler->in_flight.fetch_sub(1, std::memory_order_acq_rel) == 1)
                handler->in_flight.notify_all();
            throw;
        }
        dispatching_handler = previous;

        if (handler->in_flight.fetch_sub(1, std::memory_order_acq_rel) == 1)
            handler->in_flight.notify_all();
    }
}

size_t WebsocketManager::dispatch(std::size_t max_count) {
    if (!user_thread_dispatch_)
        return 0;
    size_t scanned = 0;
    size_t dispatched = 0;
    while (scanned < max_count) {
        auto rec = mux_.read(consumer_cursor_);
        if (!rec) {
            break;
        }
        ++scanned;
        if (rec.producer_id != producer_id_) {
            continue;
        }
        dispatch_message(reinterpret_cast<const char*>(rec.data), rec.length);
        ++dispatched;
    }
    return dispatched;
}

bool WebsocketManager::dispatch(uint32_t producer_id, const char* data, std::size_t length) {
    if (!user_thread_dispatch_ || producer_id != producer_id_)
        return false;
    dispatch_message(data, length);
    return true;
}

void WebsocketManager::on_error(std::string err) {
    LOG_ERROR("[WebsocketManager] error: {}", err);
    ws_->close();
}

// Ping loop / reconnect manager

void WebsocketManager::ping_loop() {
    static constexpr auto kPingInterval        = std::chrono::seconds(50);
    static constexpr auto kCheckInterval       = std::chrono::milliseconds(100);
    static constexpr auto kInitialRetryDelay   = std::chrono::seconds(1);
    static constexpr auto kMaxRetryDelay       = std::chrono::seconds(30);
    static const std::string ping_msg          = R"({"method":"ping"})";

    auto next_ping    = std::chrono::steady_clock::now() + kPingInterval;
    auto retry_delay  = kInitialRetryDelay;
    bool reconnect_scheduled = false;
    std::chrono::steady_clock::time_point reconnect_at;
    bool was_connected = false;

    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kCheckInterval);
        if (!running_.load(std::memory_order_acquire))
            break;

        const auto now          = std::chrono::steady_clock::now();
        const bool is_connected = connected_.load(std::memory_order_acquire);

        if (is_connected) {
            if (!was_connected) {
                // Just (re)connected — reset backoff
                retry_delay          = kInitialRetryDelay;
                reconnect_scheduled  = false;
                was_connected        = true;
            }
            if (now >= next_ping) {
                ws_->send(ping_msg.c_str(), ping_msg.size());
                next_ping = now + kPingInterval;
            }
            continue;
        }

        was_connected = false;

        // Wait until the socket is fully DISCONNECTED before attempting open(),
        // so we don't interrupt an in-progress connection attempt.
        if (!ws_ || ws_->status() != Websocket::Status::DISCONNECTED)
            continue;

        if (!reconnect_scheduled) {
            reconnect_scheduled = true;
            reconnect_at        = now + retry_delay;
            LOG_INFO("[WebsocketManager] disconnected — reconnecting in {}ms",
                     std::chrono::duration_cast<std::chrono::milliseconds>(retry_delay).count());
            retry_delay = std::min(retry_delay * 2, kMaxRetryDelay);
            continue;
        }

        if (now >= reconnect_at) {
            LOG_INFO("[WebsocketManager] reconnecting...");
            reconnect_scheduled = false;
            ws_->open();
            next_ping = now + kPingInterval;
        }
    }
}

// Subscribe / unsubscribe

int WebsocketManager::subscribe(
    const nlohmann::json& subscription,
    std::function<void(const nlohmann::json&)> callback)
{
    const int id = next_id_.fetch_add(1);
    const std::string identifier = to_identifier(subscription);
    const auto state = subscription_state_for(identifier, subscription);
    const auto handler = std::make_shared<Handler>(id, std::move(callback), state);

    state->handler_count.fetch_add(1, std::memory_order_acq_rel);

    try {
        auto old = std::atomic_load_explicit(&handlers_, std::memory_order_acquire);
        std::shared_ptr<HandlerMap> new_map;
        do {
            if (is_single_subscriber_channel(identifier)) {
                const auto existing = old->find(identifier);
                if (existing != old->end() && !existing->second.empty())
                    throw std::runtime_error("Channel does not support multiple subscriptions: " + identifier);
            }

            new_map = std::make_shared<HandlerMap>(*old);
            (*new_map)[identifier].push_back(handler);
        } while (!std::atomic_compare_exchange_weak_explicit(
            &handlers_, &old,
            std::static_pointer_cast<const HandlerMap>(new_map),
            std::memory_order_release,
            std::memory_order_acquire));
    } catch (...) {
        if (state->handler_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
            deactivate_subscription(state, subscription);
        throw;
    }

    activate_subscription(state);
    return id;
}

void WebsocketManager::unsubscribe(
    const nlohmann::json& subscription, int subscription_id)
{
    const std::string identifier = to_identifier(subscription);
    HandlerPtr removed_handler;

    auto old = std::atomic_load_explicit(&handlers_, std::memory_order_acquire);
    for (;;) {
        auto new_map = std::make_shared<HandlerMap>(*old);
        removed_handler.reset();

        const auto it = new_map->find(identifier);
        if (it != new_map->end()) {
            auto& handlers = it->second;
            handlers.erase(
                std::remove_if(handlers.begin(), handlers.end(),
                               [&](const HandlerPtr& handler) {
                                   if (handler->subscription_id != subscription_id)
                                       return false;
                                   removed_handler = handler;
                                   return true;
                               }),
                handlers.end());
            if (handlers.empty()) {
                new_map->erase(it);
            }
        }

        if (std::atomic_compare_exchange_weak_explicit(
                &handlers_, &old,
                std::static_pointer_cast<const HandlerMap>(new_map),
                std::memory_order_release,
                std::memory_order_acquire))
            break;
    }

    if (!removed_handler)
        return;

    removed_handler->active.store(false, std::memory_order_release);

    const auto state = removed_handler->subscription_state;
    if (state->handler_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
        deactivate_subscription(state, subscription);

    if (dispatching_handler == removed_handler.get())
        return;

    wait_for_zero(removed_handler->in_flight);
}

} // namespace hyperliquid
