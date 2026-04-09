#include "core/context_bus.h"

#include <any>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

using vectis::core::ContextBus;
using vectis::core::ContextData;

TEST(ContextBusTest, PublishBeforeSubscribe_Noop)
{
    ContextBus bus;
    // Nothing subscribed yet — publishing must not throw, assert, or crash.
    bus.publish("topic.noop", ContextData{std::string{"payload"}});
    SUCCEED();
}

TEST(ContextBusTest, SubscribeThenPublish_CallsCallback)
{
    ContextBus  bus;
    std::string received;
    bus.subscribe("topic.alpha", [&](const ContextData& data) {
        received = std::any_cast<std::string>(data);
    });
    bus.publish("topic.alpha", ContextData{std::string{"hello"}});
    EXPECT_EQ(received, "hello");
}

TEST(ContextBusTest, Unsubscribe_StopsDelivery)
{
    ContextBus bus;
    int        call_count = 0;
    const auto id = bus.subscribe("topic.beta", [&](const ContextData&) { ++call_count; });

    bus.publish("topic.beta", ContextData{});
    EXPECT_EQ(call_count, 1);

    bus.unsubscribe(id);
    bus.publish("topic.beta", ContextData{});
    EXPECT_EQ(call_count, 1) << "callback fired after unsubscribe";
}

TEST(ContextBusTest, TwoThreads_SmokeTest)
{
    // Stress publish/subscribe/unsubscribe from two threads for a short
    // burst. We're not checking deterministic delivery — only that the
    // bus does not crash, deadlock, or corrupt its internal state under
    // concurrent use.
    ContextBus              bus;
    std::atomic<bool>       stop{false};
    std::atomic<int>        total_calls{0};
    constexpr int           k_iters = 500;

    auto pub_worker = [&] {
        for (int i = 0; i < k_iters && !stop.load(); ++i) {
            bus.publish("topic.stress", ContextData{i});
        }
    };

    auto sub_worker = [&] {
        std::vector<std::uint64_t> ids;
        for (int i = 0; i < k_iters && !stop.load(); ++i) {
            const auto id = bus.subscribe(
                "topic.stress", [&](const ContextData&) { ++total_calls; });
            ids.push_back(id);
            if (ids.size() > 4) {
                bus.unsubscribe(ids.front());
                ids.erase(ids.begin());
            }
        }
        for (const auto id : ids) {
            bus.unsubscribe(id);
        }
    };

    std::thread t1(pub_worker);
    std::thread t2(sub_worker);

    // Safety timeout — the workers should finish well before this.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    t1.join();
    t2.join();
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "workers exceeded deadline";

    // At least some deliveries should have happened — but the exact count
    // is non-deterministic. We only require "no crash" to pass.
    SUCCEED();
}

} // namespace
