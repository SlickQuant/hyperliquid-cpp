#include <gtest/gtest.h>
#include <hyperliquid/info.hpp>
#include <hyperliquid/utils/constants.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

using hyperliquid::Info;
using hyperliquid::TESTNET_API_URL;
using json = nlohmann::json;

static constexpr std::chrono::seconds kMsgTimeout{15};

// ── Synchronization helper ────────────────────────────────────────────────────

struct Waiter {
    std::mutex mtx;
    std::condition_variable cv;
    bool received = false;
    json msg;

    // Called from the WebSocket thread; safe to call multiple times.
    void set(const json& m) {
        { std::lock_guard lock(mtx); received = true; msg = m; }
        cv.notify_one();
    }

    bool wait(std::chrono::seconds timeout = kMsgTimeout) {
        std::unique_lock lock(mtx);
        return cv.wait_for(lock, timeout, [this] { return received; });
    }
};

// ── Fixture ───────────────────────────────────────────────────────────────────
// One shared Info (and therefore one WebSocket connection) for the whole suite.
// All sync objects are heap-allocated via shared_ptr so there are no dangling
// references if a test fails before it can explicitly unsubscribe.

class WsIntegration : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        info_ = std::make_unique<Info>(TESTNET_API_URL, /*skip_ws=*/false);
        // Give the connection a moment to complete the opening handshake
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    static void TearDownTestSuite() {
        info_.reset();
    }

    static std::unique_ptr<Info> info_;
};

std::unique_ptr<Info> WsIntegration::info_;

// ── allMids ───────────────────────────────────────────────────────────────────
// allMids fires on every top-of-book change across all markets — the most
// reliable channel for confirming the connection is alive.

TEST_F(WsIntegration, AllMidsReceivesMessage) {
    auto w = std::make_shared<Waiter>();
    const json kSub{{"type", "allMids"}};

    int sid = info_->subscribe(kSub, [w](const json& m) { w->set(m); });
    bool ok = w->wait();
    info_->unsubscribe(kSub, sid);

    ASSERT_TRUE(ok) << "Timed out waiting for allMids message";
}

TEST_F(WsIntegration, AllMidsChannelField) {
    auto w = std::make_shared<Waiter>();
    const json kSub{{"type", "allMids"}};

    int sid = info_->subscribe(kSub, [w](const json& m) { w->set(m); });
    bool ok = w->wait();
    info_->unsubscribe(kSub, sid);

    ASSERT_TRUE(ok) << "Timed out";
    ASSERT_TRUE(w->msg.contains("channel")) << w->msg.dump();
    EXPECT_EQ(w->msg["channel"].get<std::string>(), "allMids");
}

TEST_F(WsIntegration, AllMidsMessageHasDataField) {
    auto w = std::make_shared<Waiter>();
    const json kSub{{"type", "allMids"}};

    int sid = info_->subscribe(kSub, [w](const json& m) { w->set(m); });
    bool ok = w->wait();
    info_->unsubscribe(kSub, sid);

    ASSERT_TRUE(ok) << "Timed out";
    EXPECT_TRUE(w->msg.contains("data")) << w->msg.dump();
}

// ── l2Book ────────────────────────────────────────────────────────────────────

TEST_F(WsIntegration, L2BookReceivesMessage) {
    auto w = std::make_shared<Waiter>();
    const json kSub{{"type", "l2Book"}, {"coin", "ETH"}};

    int sid = info_->subscribe(kSub, [w](const json& m) { w->set(m); });
    bool ok = w->wait();
    info_->unsubscribe(kSub, sid);

    ASSERT_TRUE(ok) << "Timed out waiting for l2Book:ETH message";
}

TEST_F(WsIntegration, L2BookChannelField) {
    auto w = std::make_shared<Waiter>();
    const json kSub{{"type", "l2Book"}, {"coin", "ETH"}};

    int sid = info_->subscribe(kSub, [w](const json& m) { w->set(m); });
    bool ok = w->wait();
    info_->unsubscribe(kSub, sid);

    ASSERT_TRUE(ok) << "Timed out";
    ASSERT_TRUE(w->msg.contains("channel")) << w->msg.dump();
    EXPECT_EQ(w->msg["channel"].get<std::string>(), "l2Book");
}

TEST_F(WsIntegration, L2BookDataHasCoin) {
    auto w = std::make_shared<Waiter>();
    const json kSub{{"type", "l2Book"}, {"coin", "ETH"}};

    int sid = info_->subscribe(kSub, [w](const json& m) { w->set(m); });
    bool ok = w->wait();
    info_->unsubscribe(kSub, sid);

    ASSERT_TRUE(ok) << "Timed out";
    ASSERT_TRUE(w->msg.contains("data")) << w->msg.dump();
    ASSERT_TRUE(w->msg["data"].contains("coin")) << w->msg["data"].dump();
    EXPECT_EQ(w->msg["data"]["coin"].get<std::string>(), "ETH");
}

TEST_F(WsIntegration, L2BookDataHasLevels) {
    auto w = std::make_shared<Waiter>();
    const json kSub{{"type", "l2Book"}, {"coin", "ETH"}};

    int sid = info_->subscribe(kSub, [w](const json& m) { w->set(m); });
    bool ok = w->wait();
    info_->unsubscribe(kSub, sid);

    ASSERT_TRUE(ok) << "Timed out";
    ASSERT_TRUE(w->msg["data"].contains("levels")) << w->msg["data"].dump();
    // levels is always [bids, asks] — two elements
    EXPECT_EQ(w->msg["data"]["levels"].size(), 2u);
}

TEST_F(WsIntegration, L2BookDifferentCoin) {
    auto w = std::make_shared<Waiter>();
    const json kSub{{"type", "l2Book"}, {"coin", "BTC"}};

    int sid = info_->subscribe(kSub, [w](const json& m) { w->set(m); });
    bool ok = w->wait();
    info_->unsubscribe(kSub, sid);

    ASSERT_TRUE(ok) << "Timed out waiting for l2Book:BTC";
    EXPECT_EQ(w->msg["data"]["coin"].get<std::string>(), "BTC");
}

// ── Subscription IDs ──────────────────────────────────────────────────────────

TEST_F(WsIntegration, SubscribeReturnsPositiveId) {
    const json kSub{{"type", "allMids"}};
    int sid = info_->subscribe(kSub, [](const json&) {});
    EXPECT_GT(sid, 0);
    info_->unsubscribe(kSub, sid);
}

TEST_F(WsIntegration, TwoSubscriptionsHaveDistinctIds) {
    const json kSub1{{"type", "allMids"}};
    const json kSub2{{"type", "l2Book"}, {"coin", "ETH"}};

    int sid1 = info_->subscribe(kSub1, [](const json&) {});
    int sid2 = info_->subscribe(kSub2, [](const json&) {});

    EXPECT_NE(sid1, sid2);

    info_->unsubscribe(kSub1, sid1);
    info_->unsubscribe(kSub2, sid2);
}

TEST_F(WsIntegration, SameChannelTwoSubscriptionsHaveDistinctIds) {
    const json kSub{{"type", "allMids"}};

    int sid1 = info_->subscribe(kSub, [](const json&) {});
    int sid2 = info_->subscribe(kSub, [](const json&) {});

    EXPECT_NE(sid1, sid2);

    info_->unsubscribe(kSub, sid1);
    info_->unsubscribe(kSub, sid2);
}

// ── Multiple callbacks on the same channel ────────────────────────────────────

TEST_F(WsIntegration, TwoCallbacksSameChannel) {
    auto w1 = std::make_shared<Waiter>();
    auto w2 = std::make_shared<Waiter>();
    const json kSub{{"type", "allMids"}};

    int sid1 = info_->subscribe(kSub, [w1](const json& m) { w1->set(m); });
    int sid2 = info_->subscribe(kSub, [w2](const json& m) { w2->set(m); });

    bool ok1 = w1->wait();
    bool ok2 = w2->wait();

    info_->unsubscribe(kSub, sid1);
    info_->unsubscribe(kSub, sid2);

    EXPECT_TRUE(ok1) << "Callback 1 timed out";
    EXPECT_TRUE(ok2) << "Callback 2 timed out";
}

// ── Simultaneous subscriptions on different channels ─────────────────────────

TEST_F(WsIntegration, TwoChannelsSimultaneous) {
    auto w_mids = std::make_shared<Waiter>();
    auto w_book = std::make_shared<Waiter>();
    const json kSubMids{{"type", "allMids"}};
    const json kSubBook{{"type", "l2Book"}, {"coin", "ETH"}};

    int sid_mids = info_->subscribe(kSubMids, [w_mids](const json& m) { w_mids->set(m); });
    int sid_book = info_->subscribe(kSubBook, [w_book](const json& m) { w_book->set(m); });

    bool ok_mids = w_mids->wait();
    bool ok_book = w_book->wait();

    info_->unsubscribe(kSubMids, sid_mids);
    info_->unsubscribe(kSubBook, sid_book);

    EXPECT_TRUE(ok_mids) << "allMids timed out";
    EXPECT_TRUE(ok_book) << "l2Book:ETH timed out";
}

// ── Unsubscribe stops delivery ─────────────────────────────────────────────────

TEST_F(WsIntegration, UnsubscribeStopsDelivery) {
    auto w       = std::make_shared<Waiter>();
    auto counter = std::make_shared<std::atomic<int>>(0);
    const json kSub{{"type", "allMids"}};

    int sid = info_->subscribe(kSub, [w, counter](const json& m) {
        ++(*counter);
        w->set(m);
    });

    // Confirm messages arrive before unsubscribing
    ASSERT_TRUE(w->wait()) << "No messages before unsubscribe — connection issue?";

    info_->unsubscribe(kSub, sid);
    int count_at_unsub = counter->load();

    // Wait to catch any further deliveries.
    // Allow +1 for a message already in-flight when unsubscribe was called.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_LE(counter->load() - count_at_unsub, 1);
}

// ── unsubscribe blocks until an in-flight callback returns ────────────────────
// Regression test for the race: on_message copies callbacks under the lock,
// releases the lock, then invokes them.  Without the fix, a concurrent
// unsubscribe() could return (and the owner could be destroyed) before the
// copied callback fired, causing a use-after-free on the captured this pointer.
// With the fix, unsubscribe() must not return until the in-flight callback has
// fully returned.

TEST_F(WsIntegration, UnsubscribeBlocksUntilCallbackCompletes) {
    std::mutex cb_mtx;
    std::condition_variable cb_cv;
    bool entered  = false;
    bool may_exit = false;

    const json kSub{{"type", "allMids"}};
    int sid = info_->subscribe(kSub, [&](const json&) {
        { std::lock_guard lk(cb_mtx); entered = true; }
        cb_cv.notify_all();
        // Hold here to simulate a slow callback; the test releases us below.
        std::unique_lock lk(cb_mtx);
        cb_cv.wait(lk, [&] { return may_exit; });
    });

    // Wait for the first dispatch to enter the callback body.
    bool started;
    {
        std::unique_lock lk(cb_mtx);
        started = cb_cv.wait_for(lk, kMsgTimeout, [&] { return entered; });
    }

    // Call unsubscribe from a second thread while the callback is still
    // executing inside its body.
    std::atomic<bool> unsub_done{false};
    std::thread unsub_thr([&] {
        info_->unsubscribe(kSub, sid);
        unsub_done = true;
    });

    // Give the unsubscribe thread time to reach its internal wait-for-drain path.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool still_blocked = !unsub_done.load();

    // Release the blocking callback, then wait for everything to drain.
    { std::lock_guard lk(cb_mtx); may_exit = true; }
    cb_cv.notify_all();
    unsub_thr.join();

    ASSERT_TRUE(started)       << "allMids callback never started — connection issue?";
    EXPECT_TRUE(still_blocked) << "unsubscribe returned before in-flight callback finished";
    EXPECT_TRUE(unsub_done.load());
}

// ── callback can remove a later callback from the same dispatch ───────────────
// Regression test for the remaining snapshot hazard: callback A unsubscribes
// callback B while both are present in the same loaded snapshot. B must be
// skipped before invocation, not merely removed from future snapshots.

TEST_F(WsIntegration, CallbackCanRemoveLaterCallbackFromSameDispatch) {
    auto w = std::make_shared<Waiter>();
    const json kSub{{"type", "allMids"}};

    std::atomic<bool> armed{false};
    std::atomic<bool> removed{false};
    std::atomic<int> b_calls{0};

    int sid_b = 0;
    int sid_a = info_->subscribe(kSub, [&](const json& m) {
        if (!armed.load(std::memory_order_acquire))
            return;
        if (!removed.exchange(true, std::memory_order_acq_rel))
            info_->unsubscribe(kSub, sid_b);
        w->set(m);
    });
    sid_b = info_->subscribe(kSub, [&](const json&) {
        if (armed.load(std::memory_order_acquire))
            ++b_calls;
    });

    armed.store(true, std::memory_order_release);
    ASSERT_TRUE(w->wait()) << "Timed out waiting for allMids dispatch";

    // Give the IO thread a moment to finish the same dispatch and confirm that
    // sid_b was not invoked after sid_a removed it.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(b_calls.load(), 0) << "Later callback still fired after mid-dispatch removal";

    info_->unsubscribe(kSub, sid_a);
}

// ── Partial unsubscribe (one of two callbacks removed) ────────────────────────

TEST_F(WsIntegration, UnsubscribeOneCallbackLeavesOther) {
    auto w1      = std::make_shared<Waiter>();
    auto counter2 = std::make_shared<std::atomic<int>>(0);
    const json kSub{{"type", "allMids"}};

    int sid1 = info_->subscribe(kSub, [w1](const json& m) { w1->set(m); });
    int sid2 = info_->subscribe(kSub, [counter2](const json&) { ++(*counter2); });

    // Wait until both have received at least one message
    ASSERT_TRUE(w1->wait()) << "Timed out before partial unsubscribe";
    // Give sid2's callback time to fire too
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Remove only sid1
    info_->unsubscribe(kSub, sid1);
    int snap = counter2->load();

    // sid2 must continue to receive messages for another second
    std::this_thread::sleep_for(std::chrono::seconds(1));
    EXPECT_GT(counter2->load(), snap) << "sid2 stopped receiving after sid1 was removed";

    info_->unsubscribe(kSub, sid2);
}

// ── Subscribe / unsubscribe lifecycle (no message requirement) ────────────────
// For channels that may be quiet on testnet — just verify no crash.

TEST_F(WsIntegration, TradesSubscribeCycle) {
    const json kSub{{"type", "trades"}, {"coin", "ETH"}};
    int sid = info_->subscribe(kSub, [](const json&) {});
    EXPECT_GT(sid, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    info_->unsubscribe(kSub, sid);
}

TEST_F(WsIntegration, CandleSubscribeCycle) {
    const json kSub{{"type", "candle"}, {"coin", "BTC"}, {"interval", "1m"}};
    int sid = info_->subscribe(kSub, [](const json&) {});
    EXPECT_GT(sid, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    info_->unsubscribe(kSub, sid);
}

TEST_F(WsIntegration, DoubleUnsubscribeIsHarmless) {
    const json kSub{{"type", "allMids"}};
    int sid = info_->subscribe(kSub, [](const json&) {});
    info_->unsubscribe(kSub, sid);
    // Second call must not throw or crash
    EXPECT_NO_THROW(info_->unsubscribe(kSub, sid));
}
