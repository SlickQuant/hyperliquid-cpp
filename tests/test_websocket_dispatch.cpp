#include <gtest/gtest.h>

#include <hyperliquid/websocket_manager.hpp>

#include <slick/stream_buffer_multiplexer.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using hyperliquid::WebsocketManager;

namespace {

constexpr const char* kLocalBaseUrl = "http://127.0.0.1:1";

std::string unique_name(const char* prefix) {
    static std::atomic_uint counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string(prefix) + "_" + std::to_string(stamp) + "_" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

void publish(slick::stream_buffer_multiplexer::producer_buffer& producer,
             std::string_view message)
{
    auto [buffer, size] = producer.prepare(message.size());
    ASSERT_GE(size, message.size());
    std::memcpy(buffer, message.data(), message.size());
    producer.commit(message.size());
    producer.consume(message.size());
}

std::string all_mids_message() {
    return R"({"channel":"allMids","data":{"ETH":"1000.0"}})";
}

} // namespace

TEST(UserThreadDispatch, DispatchIgnoresForeignProducerRecordsAndReturnsDispatchedCount) {
    slick::stream_buffer_multiplexer mux(16);
    auto foreign = mux.add_producer(0, 4096, 16);

    WebsocketManager manager(
        kLocalBaseUrl,
        mux,
        4096,
        16,
        nullptr,
        1024,
        true);

    ASSERT_NE(manager.producer_id(), foreign->producer_id());

    std::atomic_int callbacks{0};
    const int sid = manager.subscribe({{"type", "allMids"}}, [&](const nlohmann::json&) {
        callbacks.fetch_add(1, std::memory_order_relaxed);
    });

    publish(*foreign, all_mids_message());
    publish(*foreign, all_mids_message());
    publish(*foreign, all_mids_message());

    EXPECT_EQ(manager.dispatch(2), 0u);
    EXPECT_EQ(callbacks.load(std::memory_order_relaxed), 0);

    manager.unsubscribe({{"type", "allMids"}}, sid);
}

TEST(UserThreadDispatch, DispatchRoutesMatchingProducerRecordsToCallbacks) {
    slick::stream_buffer_multiplexer mux(16);

    WebsocketManager manager(
        kLocalBaseUrl,
        mux,
        4096,
        16,
        nullptr,
        1024,
        true);

    std::atomic_int callbacks{0};
    const int sid = manager.subscribe({{"type", "allMids"}}, [&](const nlohmann::json& msg) {
        if (msg.value("channel", "") == "allMids")
            callbacks.fetch_add(1, std::memory_order_relaxed);
    });

    const std::string payload = all_mids_message();
    ASSERT_FALSE(manager.dispatch(manager.producer_id() + 1, payload.data(), payload.size()));
    ASSERT_TRUE(manager.dispatch(manager.producer_id(), payload.data(), payload.size()));
    EXPECT_EQ(callbacks.load(std::memory_order_relaxed), 1);

    manager.unsubscribe({{"type", "allMids"}}, sid);
}

TEST(StreamBufferMultiplexer, SharedMemoryReaderOpensExistingWriterSegments) {
    const std::string mux_name = unique_name("hl_cpp_mux");
    const std::string buffer_name = unique_name("hl_cpp_buf");
    const std::string payload = all_mids_message();

    slick::stream_buffer_multiplexer writer(16, mux_name.c_str());
    auto writer_producer = writer.add_producer(0, 4096, 16, buffer_name.c_str());

    slick::stream_buffer_multiplexer reader(mux_name.c_str());
    reader.add_producer(0, buffer_name.c_str());
    uint64_t cursor = reader.initial_reading_index();

    publish(*writer_producer, payload);

    const auto record = reader.read(cursor);
    ASSERT_TRUE(record);
    EXPECT_EQ(record.producer_id, 0u);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(record.data), record.length),
              payload);
}
