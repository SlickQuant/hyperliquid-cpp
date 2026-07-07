// WebSocket market data example: subscribe to public mainnet updates.
// Usage: market_data_websocket [coin ...] [--seconds N]
// Defaults: coins=ETH, seconds=30

#include <hyperliquid/hyperliquid.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

static constexpr const char* SHM_MUX_QUEUE_NAME = "mux_queue";
static constexpr const char* SHM_MD_BUF_NAME    = "md_buf";
static constexpr uint32_t mux_record_size = 1 << 20;
static constexpr uint32_t read_buffer_size = 1 << 24;
static constexpr uint32_t read_control_size = 1 << 16;

struct Options {
    std::vector<std::string> coins{"ETH"};
    int seconds = 30;
};

int parse_positive_int(const std::string& value) {
    std::size_t parsed = 0;
    const int result = std::stoi(value, &parsed);
    if (parsed != value.size() || result <= 0)
        throw std::invalid_argument("expected a positive integer");
    return result;
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    std::vector<std::string> coins;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seconds") {
            if (i + 1 >= argc)
                throw std::invalid_argument("--seconds requires a value");
            options.seconds = parse_positive_int(argv[++i]);
        } else {
            coins.push_back(arg);
        }
    }

    if (!coins.empty())
        options.coins = std::move(coins);
    return options;
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

    std::cout << "[l2Book] " << coin
              << " best_bid=" << bid
              << " best_ask=" << ask << "\n";
}

std::string mid_price_from_message(const nlohmann::json& msg, const std::string& coin) {
    if (!msg.contains("data") || !msg["data"].is_object())
        return {};

    const auto& data = msg["data"];
    if (data.contains(coin) && data[coin].is_string())
        return data[coin].get<std::string>();

    if (data.contains("mids") && data["mids"].is_object() &&
        data["mids"].contains(coin) && data["mids"][coin].is_string())
        return data["mids"][coin].get<std::string>();

    return {};
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    try {
        options = parse_options(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "Invalid arguments: " << ex.what() << "\n"
                  << "Usage: market_data_websocket [coin ...] [--seconds N]\n";
        return 1;
    }

    hyperliquid::Info info(
        hyperliquid::MAINNET_API_URL,
        /*skip_ws=*/false,
        /*user_thread_dispatch*/false,
        mux_record_size,
        SHM_MUX_QUEUE_NAME,
        read_buffer_size,
        read_control_size,
        SHM_MD_BUF_NAME
    );

    std::mutex cout_mutex;
    std::atomic<int> all_mids_updates{0};
    std::vector<std::shared_ptr<std::atomic<int>>> l2_book_updates;
    l2_book_updates.reserve(options.coins.size());
    for (std::size_t i = 0; i < options.coins.size(); ++i)
        l2_book_updates.push_back(std::make_shared<std::atomic<int>>(0));

    const nlohmann::json all_mids_sub{{"type", "allMids"}};

    const int all_mids_id = info.subscribe(all_mids_sub, [&](const nlohmann::json& msg) {
        const int update_count = all_mids_updates.fetch_add(1, std::memory_order_relaxed) + 1;

        std::lock_guard lock(cout_mutex);
        std::cout << "[allMids] update #" << update_count;
        for (const auto& coin : options.coins) {
            const std::string mid_price = mid_price_from_message(msg, coin);
            if (!mid_price.empty())
                std::cout << " " << coin << "=" << mid_price;
        }
        std::cout << "\n";
    });

    std::vector<std::pair<nlohmann::json, int>> l2_book_subscriptions;
    l2_book_subscriptions.reserve(options.coins.size());

    for (std::size_t i = 0; i < options.coins.size(); ++i) {
        const std::string coin = options.coins[i];
        const nlohmann::json subscription{{"type", "l2Book"}, {"coin", coin}};
        const int id = info.subscribe(subscription, [&, i, coin](const nlohmann::json& msg) {
            l2_book_updates[i]->fetch_add(1, std::memory_order_relaxed);

            std::lock_guard lock(cout_mutex);
            print_top_of_book(msg, coin);
        });
        l2_book_subscriptions.emplace_back(subscription, id);
    }

    {
        std::lock_guard lock(cout_mutex);
        std::cout << "Listening for";
        for (const auto& coin : options.coins)
            std::cout << " " << coin;
        std::cout << " market data on mainnet for " << options.seconds << " seconds...\n";
    }

    std::this_thread::sleep_for(std::chrono::seconds(options.seconds));

    info.unsubscribe(all_mids_sub, all_mids_id);
    for (const auto& [subscription, id] : l2_book_subscriptions)
        info.unsubscribe(subscription, id);

    std::cout << "Done. Received "
              << all_mids_updates.load(std::memory_order_relaxed)
              << " allMids updates";
    for (std::size_t i = 0; i < options.coins.size(); ++i) {
        std::cout << ", " << l2_book_updates[i]->load(std::memory_order_relaxed)
                  << " " << options.coins[i] << " l2Book updates";
    }
    std::cout << ".\n";

    return 0;
}
