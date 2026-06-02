#include <hyperliquid/websocket_manager.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <slick/net/websocket.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

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
    connected_ = true;
    flush_pending();
}

void WebsocketManager::on_disconnected() {
    connected_ = false;
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

    std::vector<std::function<void(const nlohmann::json&)>> cbs;
    {
        std::lock_guard lock(mutex_);
        auto it = handlers_.find(identifier);
        if (it == handlers_.end()) {
            // Try without suffix (e.g. allMids has no coin)
            it = handlers_.find(channel);
        }
        if (it != handlers_.end()) {
            for (const auto& [id, cb] : it->second)
                cbs.push_back(cb);
        }
    }

    for (auto& cb : cbs) cb(msg);
}

void WebsocketManager::on_error(std::string err) {
    std::cerr << "[WebsocketManager] error: " << err << "\n";
}

// ── Ping loop ─────────────────────────────────────────────────────────────────

void WebsocketManager::ping_loop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (connected_ && ws_) {
            static const std::string ping_msg = R"({"method":"ping"})";
            ws_->send(ping_msg.c_str(), ping_msg.size());
        }
    }
}

// ── Pending subscription flush ────────────────────────────────────────────────

void WebsocketManager::flush_pending() {
    std::vector<nlohmann::json> subs;
    {
        std::lock_guard lock(mutex_);
        subs = std::move(pending_subs_);
        pending_subs_.clear();
    }
    for (const auto& sub : subs) {
        nlohmann::json msg{{"method", "subscribe"}, {"subscription", sub}};
        std::string s = msg.dump();
        ws_->send(s.c_str(), s.size());
    }
}

// ── Subscribe / unsubscribe ───────────────────────────────────────────────────

int WebsocketManager::subscribe(
    const nlohmann::json& subscription,
    std::function<void(const nlohmann::json&)> callback)
{
    int id = next_id_.fetch_add(1);
    std::string ident = to_identifier(subscription);

    {
        std::lock_guard lock(mutex_);
        handlers_[ident].emplace_back(id, std::move(callback));

        if (!connected_) {
            pending_subs_.push_back(subscription);
        } else {
            nlohmann::json msg{{"method", "subscribe"}, {"subscription", subscription}};
            std::string s = msg.dump();
            ws_->send(s.c_str(), s.size());
        }
    }
    return id;
}

void WebsocketManager::unsubscribe(
    const nlohmann::json& subscription, int subscription_id)
{
    std::string ident = to_identifier(subscription);
    {
        std::lock_guard lock(mutex_);
        auto it = handlers_.find(ident);
        if (it != handlers_.end()) {
            auto& vec = it->second;
            vec.erase(
                std::remove_if(vec.begin(), vec.end(),
                    [subscription_id](const auto& p) { return p.first == subscription_id; }),
                vec.end());
            if (vec.empty()) {
                handlers_.erase(it);
                // Send unsubscribe only when no more handlers remain
                if (connected_) {
                    nlohmann::json msg{{"method", "unsubscribe"}, {"subscription", subscription}};
                    std::string s = msg.dump();
                    ws_->send(s.c_str(), s.size());
                }
            }
        }
    }
}

} // namespace hyperliquid
