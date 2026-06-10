// Basic order example: place a resting limit buy on testnet, then cancel it.
// Usage: basic_order [private_key_hex]
// If no argument is provided, falls back to the hardhat test key.

#include <hyperliquid/hyperliquid.hpp>
#include <iostream>
#include <string>

static constexpr const char* kDefaultKey =
    "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";

int main(int argc, char* argv[]) {
    const std::string private_key = (argc >= 2) ? argv[1] : kDefaultKey;

    // Create a shared Info instance (skip WebSocket for this simple example)
    auto info = std::make_shared<hyperliquid::Info>(
        hyperliquid::TESTNET_API_URL, /*skip_ws=*/true);

    // Create Exchange with the provided (or default) private key
    hyperliquid::Exchange exchange(
        private_key,
        hyperliquid::TESTNET_API_URL,
        info);

    std::cout << "Wallet: " << exchange.wallet_address() << "\n";

    // Print open positions
    auto state = info->user_state(exchange.wallet_address());
    std::cout << "Account value: "
              << state["marginSummary"]["accountValue"] << "\n";

    // Place a resting limit buy for 0.01 ETH at $1100 (well below market)
    auto result = exchange.order(
        "ETH", /*is_buy=*/true, /*sz=*/0.01, /*limit_px=*/1100.0,
        hyperliquid::LimitOrderType{hyperliquid::Tif::Gtc});

    std::cout << "Order result:\n" << result.dump(2) << "\n";

    // Cancel if it's resting
    if (result.value("status", "") == "ok") {
        const auto& statuses = result["response"]["data"]["statuses"];
        if (!statuses.empty() && statuses[0].contains("resting")) {
            int64_t oid = statuses[0]["resting"]["oid"].get<int64_t>();
            std::cout << "Order resting with oid=" << oid << ", cancelling…\n";

            auto cancel = exchange.cancel("ETH", oid);
            std::cout << "Cancel result:\n" << cancel.dump(2) << "\n";
        }
    }

    return 0;
}
