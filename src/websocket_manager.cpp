#include <hyperliquid/websocket_manager.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <slick/net/websocket.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
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

WebsocketManager::WebsocketManager(std::string_view http_base_url)
    : ws_url_(http_to_ws_url(http_base_url))
    , handlers_(std::make_shared<HandlerMap>())
{
    ws_ = std::make_unique<slick::net::Websocket>(
        ws_url_,
        [this]() { on_connected(); },
        [this]() { on_disconnected(); },
        [this](const char* data, std::size_t len) { on_message(data, len); },
        [this](std::string&& err) { on_error(std::move(err)); });

    ws_->open();
    ping_thread_ = std::thread([this]() { ping_loop(); });
}

WebsocketManager::~WebsocketManager() {
    running_.store(false, std::memory_order_release);
    if (ping_thread_.joinable())
        ping_thread_.join();
    if (ws_) {
        ws_->reset_callbacks();
        ws_->close();
    }
}

// Connection callbacks

void WebsocketManager::on_connected() {
    connected_.store(true, std::memory_order_release);
}

void WebsocketManager::on_disconnected() {
    connected_.store(false, std::memory_order_release);
}

void WebsocketManager::on_message(const char* data, std::size_t len) {
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

void WebsocketManager::on_error(std::string err) {
    std::cerr << "[WebsocketManager] error: " << err << "\n";
}

// Ping loop

void WebsocketManager::ping_loop() {
    static constexpr auto kPingInterval = std::chrono::seconds(50);
    static const std::string ping_msg = R"({"method":"ping"})";

    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kPingInterval);
        if (!running_.load(std::memory_order_acquire))
            break;
        if (connected_.load(std::memory_order_acquire) && ws_)
            ws_->send(ping_msg.c_str(), ping_msg.size());
    }
}

// Subscribe / unsubscribe

int WebsocketManager::subscribe(
    const nlohmann::json& subscription,
    std::function<void(const nlohmann::json&)> callback)
{
    const int id = next_id_.fetch_add(1);
    const std::string identifier = to_identifier(subscription);
    const auto handler = std::make_shared<Handler>(id, std::move(callback));

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

    nlohmann::json msg{{"method", "subscribe"}, {"subscription", subscription}};
    const std::string serialized = msg.dump();
    ws_->send(serialized.c_str(), serialized.size());
    return id;
}

void WebsocketManager::unsubscribe(
    const nlohmann::json& subscription, int subscription_id)
{
    const std::string identifier = to_identifier(subscription);
    HandlerPtr removed_handler;
    bool last_handler = false;

    auto old = std::atomic_load_explicit(&handlers_, std::memory_order_acquire);
    for (;;) {
        auto new_map = std::make_shared<HandlerMap>(*old);
        removed_handler.reset();
        last_handler = false;

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
                last_handler = (removed_handler != nullptr);
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

    if (last_handler) {
        nlohmann::json msg{{"method", "unsubscribe"}, {"subscription", subscription}};
        const std::string serialized = msg.dump();
        ws_->send(serialized.c_str(), serialized.size());
    }

    if (dispatching_handler == removed_handler.get())
        return;

    wait_for_zero(removed_handler->in_flight);
}

} // namespace hyperliquid
