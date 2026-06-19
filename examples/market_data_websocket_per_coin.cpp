// WebSocket market data example: one WebSocket connection per coin.
// Subscribes to ETH and BTC l2Book updates on testnet.
// Usage: market_data_websocket_per_coin [seconds]
// Defaults: seconds=30

#include <hyperliquid/hyperliquid.hpp>

#include <slick/net/logging.hpp>

#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

const char* log_level_name(slick::net::LogLevel level) {
    switch (level) {
    case slick::net::LogLevel::Trace:
        return "TRACE";
    case slick::net::LogLevel::Debug:
        return "DEBUG";
    case slick::net::LogLevel::Info:
        return "INFO";
    case slick::net::LogLevel::Warn:
        return "WARN";
    case slick::net::LogLevel::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

void configure_slick_net_logging() {
    slick::net::set_log_handler(
        [](slick::net::LogLevel level, const char* format_text, std::format_args args) {
            std::string message;
            try {
                message = std::vformat(format_text, args);
            } catch (...) {
                message = format_text ? format_text : "";
            }

            std::cout << "[" << log_level_name(level) << "] " << message << "\n";
        });
}

struct SlickNetLogHandler {
    SlickNetLogHandler() {
        configure_slick_net_logging();
    }

    ~SlickNetLogHandler() {
        slick::net::clear_log_handler();
    }
};

int parse_duration_seconds(int argc, char* argv[]) {
    if (argc < 2)
        return 30;

    std::size_t parsed = 0;
    const int seconds = std::stoi(argv[1], &parsed);
    if (parsed != std::string(argv[1]).size() || seconds <= 0)
        throw std::invalid_argument("expected a positive integer duration");
    return seconds;
}

void print_top_of_book(const nlohmann::json& msg, const std::string& coin) {
    if (!msg.contains("data"))
        return;

    const auto& data = msg["data"];
    if (!data.contains("levels") || !data["levels"].is_array() || data["levels"].size() < 2)
        return;

    const auto& bids = data["levels"][0];
    const auto& asks = data["levels"][1];
    const std::string bid = (!bids.empty() && bids[0].contains("px"))
        ? bids[0]["px"].get<std::string>()
        : "n/a";
    const std::string ask = (!asks.empty() && asks[0].contains("px"))
        ? asks[0]["px"].get<std::string>()
        : "n/a";

    LOG_INFO("[l2Book] {} best_bid={} best_ask={}", coin, bid, ask);
}

struct CoinFeed {
    std::string coin;
    hyperliquid::Info info;
    nlohmann::json subscription;
    int subscription_id = 0;
    std::atomic<int> updates{0};

    explicit CoinFeed(std::string coin_)
        : coin(std::move(coin_))
        , info(hyperliquid::TESTNET_API_URL, /*skip_ws=*/false)
        , subscription{{"type", "l2Book"}, {"coin", coin}}
    {}
};

} // namespace

int main(int argc, char* argv[]) {
    const SlickNetLogHandler log_handler;

    int seconds = 30;
    try {
        seconds = parse_duration_seconds(argc, argv);
    } catch (const std::exception& ex) {
        LOG_ERROR("Invalid arguments: {}", ex.what());
        LOG_INFO("Usage: market_data_websocket_per_coin [seconds]");
        return 1;
    }

    std::vector<std::unique_ptr<CoinFeed>> feeds;
    feeds.push_back(std::make_unique<CoinFeed>("ETH"));
    feeds.push_back(std::make_unique<CoinFeed>("BTC"));

    for (auto& feed : feeds) {
        feed->subscription_id = feed->info.subscribe(
            feed->subscription,
            [feed_ptr = feed.get()](const nlohmann::json& msg) {
                feed_ptr->updates.fetch_add(1, std::memory_order_relaxed);
                print_top_of_book(msg, feed_ptr->coin);
            });
    }

    LOG_INFO("Listening for ETH and BTC market data on testnet for {} seconds using one WebSocket per coin...",
             seconds);

    std::this_thread::sleep_for(std::chrono::seconds(seconds));

    for (auto& feed : feeds)
        feed->info.unsubscribe(feed->subscription, feed->subscription_id);

    std::string summary = "Done.";
    for (const auto& feed : feeds) {
        summary += " " + feed->coin + "=" +
                   std::to_string(feed->updates.load(std::memory_order_relaxed)) +
                   " l2Book updates.";
    }
    LOG_INFO("{}", summary);

    return 0;
}
