#include <hyperliquid/websocket_manager.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <slick/net/websocket.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace {
    // Points at the callback currently executing on the WebSocket IO thread.
    // unsubscribe() uses this to avoid deadlocking when a callback removes itself.
    thread_local const void* dispatching_handler = nullptr;

    void wait_for_zero(std::atomic<unsigned int>& counter) {
        unsigned int in_flight = counter.load(std::memory_order_acquire);
        while (in_flight != 0) {
            counter.wait(in_flight, std::memory_order_relaxed);
            in_flight = counter.load(std::memory_order_acquire);
        }
    }
}

namespace hyperliquid {

// ── URL helpers ───────────────────────────────────────────────────────────────

std::string WebsocketManager::http_to_ws_url(std::string_view http_url) {
    std::string s(http_url);
    if (s.starts_with("https://")) {
        s.replace(0, 8, "wss://");
    } else if (s.starts_with("http://")) {
        s.replace(0, 7, "ws://");
    }
    // Strip trailing slash then append /ws
    while (!s.empty() && s.back() == '/') s.pop_back();
    s += "/ws";
    return s;
}

// ── Identifier mapping ────────────────────────────────────────────────────────

std::string WebsocketManager::to_identifier(const nlohmann::json& sub) {
    std::string type = sub.at("type").get<std::string>();

    if (type == "allMids")           return "allMids";
    if (type == "l2Book")            return "l2Book:" + sub.at("coin").get<std::string>();
    if (type == "bbo")               return "bbo:" + sub.at("coin").get<std::string>();
    if (type == "trades")            return "trades:" + sub.at("coin").get<std::string>();
    if (type == "candle")            return "candle:" +
                                        sub.at("coin").get<std::string>() + "," +
                                        sub.at("interval").get<std::string>();
    if (type == "userEvents")        return "userEvents:" + sub.at("user").get<std::string>();
    if (type == "userFills")         return "userFills:" + sub.at("user").get<std::string>();
    if (type == "orderUpdates")      return "orderUpdates:" + sub.at("user").get<std::string>();
    if (type == "userFundings")      return "userFundings:" + sub.at("user").get<std::string>();
    if (type == "userNonFundingLedgerUpdates")
        return "userNonFundingLedgerUpdates:" + sub.at("user").get<std::string>();
    if (type == "webData2")          return "webData2:" + sub.at("user").get<std::string>();
    if (type == "activeAssetCtx")    return "activeAssetCtx:" + sub.at("coin").get<std::string>();
    if (type == "activeAssetData")   return "activeAssetData:" +
                                        sub.at("coin").get<std::string>() + "," +
                                        sub.at("user").get<std::string>();
    return type;
}

// ── Constructor / destructor ──────────────────────────────────────────────────

WebsocketManager::WebsocketManager(std::string_view http_base_url)
    : ws_url_(http_to_ws_url(http_base_url))
    , handlers_(std::make_shared<HandlerMap>())
{
    ws_ = std::make_unique<slick::net::Websocket>(
        ws_url_,
        [this]() { on_connected(); },
        [this]() { on_disconnected(); },
        [this](const char* data, std::size_t len) { on_message(data, len); },
        [this](std::string&& err) { on_error(std::move(err)); }
    );

    ws_->open();

    ping_thread_ = std::thread([this]() { ping_loop(); });
}

WebsocketManager::~WebsocketManager() {
    running_ = false;
    if (ping_thread_.joinable()) ping_thread_.join();
    if (ws_) ws_->close();
}

// ── Connection callbacks ──────────────────────────────────────────────────────

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

    // Ignore pong
    if (msg.value("channel", "") == "pong") return;

    std::string channel = msg.value("channel", "");
    if (channel.empty()) return;

    // Map server channel name → identifier
    // (server sends e.g. {"channel":"l2Book","data":{...}}, data.coin gives full id)
    std::string identifier = channel;
    if (msg.contains("data")) {
        const auto& d = msg["data"];
        if (d.contains("coin"))
            identifier = channel + ":" + d["coin"].get<std::string>();
        else if (d.contains("user"))
            identifier = channel + ":" + d["user"].get<std::string>();
    }

    // Lock-free snapshot: atomically borrow a reference to the current map.
    // Each callback entry carries its own active/in-flight state so a callback
    // removed mid-dispatch can still be skipped before invocation.
    auto h = handlers_.load(std::memory_order_acquire);
    auto it = h->find(identifier);
    if (it == h->end())
        it = h->find(channel);   // fallback for channels without a suffix (allMids)
    if (it == h->end())
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

// ── Ping loop ─────────────────────────────────────────────────────────────────

void WebsocketManager::ping_loop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (connected_.load(std::memory_order_acquire) && ws_) {
            static const std::string ping_msg = R"({"method":"ping"})";
            ws_->send(ping_msg.c_str(), ping_msg.size());
        }
    }
}

// ── Subscribe / unsubscribe ───────────────────────────────────────────────────

int WebsocketManager::subscribe(
    const nlohmann::json& subscription,
    std::function<void(const nlohmann::json&)> callback)
{
    int id = next_id_.fetch_add(1);
    std::string ident = to_identifier(subscription);

    // CAS loop: build a new map, swap it in; retry if another writer raced us.
    auto old = handlers_.load(std::memory_order_acquire);
    auto handler = std::make_shared<Handler>(id, std::move(callback));
    std::shared_ptr<HandlerMap> new_map;
    do {
        new_map = std::make_shared<HandlerMap>(*old);
        (*new_map)[ident].push_back(handler);
    } while (!handlers_.compare_exchange_weak(
        old, new_map,
        std::memory_order_release,
        std::memory_order_acquire));

    // The WS layer buffers sends made before the connection is established,
    // so no need to track pending subscriptions ourselves.
    nlohmann::json msg{{"method", "subscribe"}, {"subscription", subscription}};
    std::string s = msg.dump();
    ws_->send(s.c_str(), s.size());
    return id;
}

void WebsocketManager::unsubscribe(
    const nlohmann::json& subscription, int subscription_id)
{
    std::string ident = to_identifier(subscription);
    HandlerPtr removed_handler;
    bool last_handler = false;

    // CAS loop: remove the entry from a fresh copy; retry on contention.
    std::shared_ptr<const HandlerMap> old = handlers_.load(std::memory_order_acquire);
    for (;;) {
        auto new_map = std::make_shared<HandlerMap>(*old);
        removed_handler.reset();
        last_handler = false;

        auto it = new_map->find(ident);
        if (it != new_map->end()) {
            auto& vec = it->second;
            vec.erase(
                std::remove_if(vec.begin(), vec.end(),
                    [&](const HandlerPtr& handler) {
                        if (handler->subscription_id != subscription_id)
                            return false;
                        removed_handler = handler;
                        return true;
                    }),
                vec.end());
            if (vec.empty()) {
                new_map->erase(it);
                last_handler = (removed_handler != nullptr);
            }
        }
        if (handlers_.compare_exchange_weak(
                old, new_map,
                std::memory_order_release,
                std::memory_order_acquire))
            break;
    }

    if (!removed_handler)
        return;

    removed_handler->active.store(false, std::memory_order_release);

    if (last_handler) {
        nlohmann::json msg{{"method", "unsubscribe"}, {"subscription", subscription}};
        std::string s = msg.dump();
        ws_->send(s.c_str(), s.size());
    }

    // Self-unsubscribe from inside the callback would deadlock waiting for our
    // own in-flight invocation to finish. Cross-callback removal remains safe:
    // later callbacks in the same snapshot re-check `active` before invoking.
    if (dispatching_handler == removed_handler.get())
        return;

    wait_for_zero(removed_handler->in_flight);
}

} // namespace hyperliquid
