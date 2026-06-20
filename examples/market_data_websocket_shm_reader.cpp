// WebSocket shared-memory reader example for market_data_websocket.
// Usage: market_data_websocket_shm_reader [--seconds N]
// Defaults: seconds=30

#include <slick/stream_buffer_multiplexer.hpp>

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

// Named shared-memory identifiers — must match market_data_websocket.cpp.
static constexpr const char* SHM_MUX_QUEUE_NAME = "mux_queue";
static constexpr const char* SHM_MD_BUF_NAME    = "md_buf";

struct Options {
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
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seconds") {
            if (i + 1 >= argc)
                throw std::invalid_argument("--seconds requires a value");
            options.seconds = parse_positive_int(argv[++i]);
        }
    }
    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    try {
        options = parse_options(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "Invalid arguments: " << ex.what() << "\n"
                  << "Usage: market_data_websocket [--seconds N]\n";
        return 1;
    }

    slick::stream_buffer_multiplexer mux(SHM_MUX_QUEUE_NAME);
    uint64_t cursor = mux.initial_reading_index();
    mux.add_producer(0, SHM_MD_BUF_NAME);

    auto stop_time = std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
    while (std::chrono::steady_clock::now() < stop_time) {
        auto record = mux.read(cursor);
        if (record) {
            std::cout << std::string_view(reinterpret_cast<const char*>(record.data), record.length);
        }
    }

    return 0;
}
