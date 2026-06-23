#include <gtest/gtest.h>

#include <hyperliquid/websocket_manager.hpp>
#include <slick/stream_buffer_multiplexer.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using hyperliquid::WebsocketManager;

namespace {

// Nothing listens on port 1, so connected_ stays false — exercises the
// "subscribe while disconnected" path introduced for reconnect support.
constexpr const char* kLocalBaseUrl = "http://127.0.0.1:1";

using tcp = boost::asio::ip::tcp;

class RecordingWebsocketServer {
public:
    RecordingWebsocketServer()
        : acceptor_(ioc_, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
        , port_(acceptor_.local_endpoint().port())
    {
        start_accept();
        io_thread_ = std::thread([this] { ioc_.run(); });
    }

    ~RecordingWebsocketServer() {
        stop_.store(true, std::memory_order_release);

        ioc_.stop();

        if (io_thread_.joinable())
            io_thread_.join();

        for (auto& session : sessions_) {
            if (session.joinable())
                session.join();
        }
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    bool wait_for_message_count(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(messages_mutex_);
        return messages_cv_.wait_for(lock, timeout, [&] {
            return messages_.size() >= count;
        });
    }

    std::vector<std::string> messages() const {
        std::lock_guard lock(messages_mutex_);
        return messages_;
    }

private:
    void start_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec && !stop_.load(std::memory_order_acquire)) {
                sessions_.emplace_back([this, socket = std::move(socket)]() mutable {
                    session_loop(std::move(socket));
                });
            }

            if (!stop_.load(std::memory_order_acquire) && acceptor_.is_open())
                start_accept();
        });
    }

    void session_loop(tcp::socket socket) {
        boost::system::error_code ec;
        boost::beast::websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept(ec);
        if (ec)
            return;

        for (;;) {
            boost::beast::flat_buffer buffer;
            ws.read(buffer, ec);
            if (ec)
                return;

            {
                std::lock_guard lock(messages_mutex_);
                messages_.push_back(boost::beast::buffers_to_string(buffer.data()));
            }
            messages_cv_.notify_all();
        }
    }

    boost::asio::io_context ioc_;
    tcp::acceptor acceptor_;
    unsigned short port_;
    std::atomic_bool stop_{false};
    std::thread io_thread_;
    std::vector<std::thread> sessions_;

    mutable std::mutex messages_mutex_;
    std::condition_variable messages_cv_;
    std::vector<std::string> messages_;
};

std::size_t count_method(const std::vector<std::string>& messages, std::string_view method) {
    std::size_t count = 0;
    for (const auto& text : messages) {
        const auto msg = nlohmann::json::parse(text);
        if (msg.value("method", "") == method)
            ++count;
    }
    return count;
}

} // namespace

// ── subscribe while disconnected routes messages via dispatch ─────────────────

TEST(SubscriptionTracking, SubscribeWhileDisconnectedDispatchFiresCallback) {
    slick::stream_buffer_multiplexer mux(16);
    WebsocketManager mgr(kLocalBaseUrl, mux, 4096, 16, nullptr, 1024, /*user_thread_dispatch=*/true);

    std::atomic_int hits{0};
    const int sid = mgr.subscribe({{"type", "allMids"}},
        [&](const nlohmann::json&) { hits.fetch_add(1, std::memory_order_relaxed); });

    const std::string msg = R"({"channel":"allMids","data":{"ETH":"1000.0"}})";
    EXPECT_TRUE(mgr.dispatch(mgr.producer_id(), msg.data(), msg.size()));
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 1);

    mgr.unsubscribe({{"type", "allMids"}}, sid);
}

// ── unsubscribe stops the callback from firing ────────────────────────────────

TEST(SubscriptionTracking, UnsubscribeStopsCallbackFiring) {
    slick::stream_buffer_multiplexer mux(16);
    WebsocketManager mgr(kLocalBaseUrl, mux, 4096, 16, nullptr, 1024, /*user_thread_dispatch=*/true);

    std::atomic_int hits{0};
    const int sid = mgr.subscribe({{"type", "allMids"}},
        [&](const nlohmann::json&) { hits.fetch_add(1, std::memory_order_relaxed); });
    mgr.unsubscribe({{"type", "allMids"}}, sid);

    const std::string msg = R"({"channel":"allMids","data":{"ETH":"1000.0"}})";
    mgr.dispatch(mgr.producer_id(), msg.data(), msg.size());
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
}

// ── multiple subscriptions are routed independently ───────────────────────────

TEST(SubscriptionTracking, MultipleChannelsRoutedIndependently) {
    slick::stream_buffer_multiplexer mux(16);
    WebsocketManager mgr(kLocalBaseUrl, mux, 4096, 16, nullptr, 1024, /*user_thread_dispatch=*/true);

    std::atomic_int allMidsHits{0}, l2BookHits{0};
    const int s1 = mgr.subscribe({{"type", "allMids"}},
        [&](const nlohmann::json&) { allMidsHits.fetch_add(1, std::memory_order_relaxed); });
    const int s2 = mgr.subscribe({{"type", "l2Book"}, {"coin", "ETH"}},
        [&](const nlohmann::json&) { l2BookHits.fetch_add(1, std::memory_order_relaxed); });

    const std::string allMidsMsg = R"({"channel":"allMids","data":{"ETH":"1000.0"}})";
    const std::string l2BookMsg  = R"({"channel":"l2Book","data":{"coin":"ETH","levels":[]}})";

    mgr.dispatch(mgr.producer_id(), allMidsMsg.data(), allMidsMsg.size());
    mgr.dispatch(mgr.producer_id(), l2BookMsg.data(),  l2BookMsg.size());
    mgr.dispatch(mgr.producer_id(), allMidsMsg.data(), allMidsMsg.size());

    EXPECT_EQ(allMidsHits.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(l2BookHits.load(std::memory_order_relaxed),  1);

    mgr.unsubscribe({{"type", "allMids"}}, s1);
    mgr.unsubscribe({{"type", "l2Book"}, {"coin", "ETH"}}, s2);
}

// ── re-subscribe after unsubscribe uses only the new callback ─────────────────
// Verifies sub_json_map_ is cleaned up on unsubscribe: only the new handler
// fires; the old (deactivated) one does not.

TEST(SubscriptionTracking, ResubscribeAfterUnsubscribeOnlyNewCallbackFires) {
    slick::stream_buffer_multiplexer mux(16);
    WebsocketManager mgr(kLocalBaseUrl, mux, 4096, 16, nullptr, 1024, /*user_thread_dispatch=*/true);

    const std::string msg = R"({"channel":"allMids","data":{"ETH":"1000.0"}})";

    std::atomic_int first{0}, second{0};
    const int s1 = mgr.subscribe({{"type", "allMids"}},
        [&](const nlohmann::json&) { first.fetch_add(1, std::memory_order_relaxed); });
    mgr.unsubscribe({{"type", "allMids"}}, s1);

    const int s2 = mgr.subscribe({{"type", "allMids"}},
        [&](const nlohmann::json&) { second.fetch_add(1, std::memory_order_relaxed); });
    mgr.dispatch(mgr.producer_id(), msg.data(), msg.size());

    EXPECT_EQ(first.load(std::memory_order_relaxed),  0);
    EXPECT_EQ(second.load(std::memory_order_relaxed), 1);

    mgr.unsubscribe({{"type", "allMids"}}, s2);
}

// ── unsubscribing one of several handlers leaves others intact ────────────────

TEST(SubscriptionTracking, UnsubscribeOneOfMultipleHandlersSameChannel) {
    slick::stream_buffer_multiplexer mux(16);
    WebsocketManager mgr(kLocalBaseUrl, mux, 4096, 16, nullptr, 1024, /*user_thread_dispatch=*/true);

    const std::string msg = R"({"channel":"allMids","data":{"ETH":"1000.0"}})";

    std::atomic_int hits1{0}, hits2{0};
    const int s1 = mgr.subscribe({{"type", "allMids"}},
        [&](const nlohmann::json&) { hits1.fetch_add(1, std::memory_order_relaxed); });
    const int s2 = mgr.subscribe({{"type", "allMids"}},
        [&](const nlohmann::json&) { hits2.fetch_add(1, std::memory_order_relaxed); });

    mgr.unsubscribe({{"type", "allMids"}}, s1);
    mgr.dispatch(mgr.producer_id(), msg.data(), msg.size());

    EXPECT_EQ(hits1.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(hits2.load(std::memory_order_relaxed), 1);

    mgr.unsubscribe({{"type", "allMids"}}, s2);
}

// ── same-channel callbacks share one server subscription ──────────────────────

TEST(SubscriptionTracking, MultipleCallbacksShareOneWireSubscriptionUntilLastUnsubscribe) {
    RecordingWebsocketServer server;
    slick::stream_buffer_multiplexer mux(16);
    const std::string base_url = server.base_url();
    WebsocketManager mgr(base_url, mux, 4096, 16, nullptr, 1024, /*user_thread_dispatch=*/true);

    const nlohmann::json sub{{"type", "allMids"}};
    const int s1 = mgr.subscribe(sub, [](const nlohmann::json&) {});
    ASSERT_TRUE(server.wait_for_message_count(1, std::chrono::seconds(5)));

    const int s2 = mgr.subscribe(sub, [](const nlohmann::json&) {});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto messages = server.messages();
    EXPECT_EQ(count_method(messages, "subscribe"), 1u);
    EXPECT_EQ(count_method(messages, "unsubscribe"), 0u);

    mgr.unsubscribe(sub, s1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    messages = server.messages();
    EXPECT_EQ(count_method(messages, "subscribe"), 1u);
    EXPECT_EQ(count_method(messages, "unsubscribe"), 0u);

    mgr.unsubscribe(sub, s2);
    ASSERT_TRUE(server.wait_for_message_count(2, std::chrono::seconds(5)));

    messages = server.messages();
    EXPECT_EQ(count_method(messages, "subscribe"), 1u);
    EXPECT_EQ(count_method(messages, "unsubscribe"), 1u);
}

// ── final unsubscribe followed by resubscribe preserves wire ordering ─────────

TEST(SubscriptionTracking, ResubscribeAfterFinalUnsubscribeSendsUnsubscribeBeforeSubscribe) {
    RecordingWebsocketServer server;
    slick::stream_buffer_multiplexer mux(16);
    const std::string base_url = server.base_url();
    WebsocketManager mgr(base_url, mux, 4096, 16, nullptr, 1024, /*user_thread_dispatch=*/true);

    const nlohmann::json sub{{"type", "allMids"}};
    int sid = mgr.subscribe(sub, [](const nlohmann::json&) {});
    ASSERT_TRUE(server.wait_for_message_count(1, std::chrono::seconds(5)));

    mgr.unsubscribe(sub, sid);
    ASSERT_TRUE(server.wait_for_message_count(2, std::chrono::seconds(5)));

    sid = mgr.subscribe(sub, [](const nlohmann::json&) {});
    ASSERT_TRUE(server.wait_for_message_count(3, std::chrono::seconds(5)));

    const auto messages = server.messages();
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(nlohmann::json::parse(messages[0]).value("method", ""), "subscribe");
    EXPECT_EQ(nlohmann::json::parse(messages[1]).value("method", ""), "unsubscribe");
    EXPECT_EQ(nlohmann::json::parse(messages[2]).value("method", ""), "subscribe");

    mgr.unsubscribe(sub, sid);
}

// ── concurrent subscribe/unsubscribe — COW must not race ─────────────────────
// Two threads subscribe to distinct channels simultaneously; no crash = no data
// race. Run under TSAN for the strongest guarantee.

TEST(SubscriptionTracking, ConcurrentSubscribeUnsubscribeNoRace) {
    slick::stream_buffer_multiplexer mux(16);
    WebsocketManager mgr(kLocalBaseUrl, mux, 4096, 16, nullptr, 1024, /*user_thread_dispatch=*/true);

    constexpr int N = 200;
    std::vector<int> ids1(N), ids2(N);

    std::thread t1([&] {
        for (int i = 0; i < N; ++i)
            ids1[i] = mgr.subscribe({{"type", "allMids"}}, [](const nlohmann::json&) {});
    });
    std::thread t2([&] {
        for (int i = 0; i < N; ++i)
            ids2[i] = mgr.subscribe({{"type", "trades"}, {"coin", "ETH"}},
                                    [](const nlohmann::json&) {});
    });
    t1.join();
    t2.join();

    for (int id : ids1) mgr.unsubscribe({{"type", "allMids"}}, id);
    for (int id : ids2) mgr.unsubscribe({{"type", "trades"}, {"coin", "ETH"}}, id);
}
