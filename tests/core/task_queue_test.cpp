#include "core/task_queue.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

namespace {

using vectis::core::CancellationToken;
using vectis::core::TaskQueue;

TEST(TaskQueueTest, Submit_TaskRunsAndCompletes)
{
    TaskQueue         q(1);
    std::atomic<bool> ran{false};

    q.submit([&](const CancellationToken&) { ran.store(true); });

    // Wait up to 1s for the task to run.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!ran.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(ran.load());
}

TEST(TaskQueueTest, CancelAll_FlagsRunningTask)
{
    TaskQueue         q(1);
    std::atomic<bool> observed_cancel{false};
    std::atomic<bool> started{false};

    q.submit([&](const CancellationToken& token) {
        started.store(true);
        // Busy-wait until cancelled or until 2s elapses.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        observed_cancel.store(token.stop_requested());
    });

    // Wait for the task to actually begin running.
    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    q.cancel_all();

    // Give the task a moment to react.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!observed_cancel.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(observed_cancel.load());
}

TEST(TaskQueueTest, DrainPending_DropsQueuedTasksBeforeRunning)
{
    TaskQueue           q(1);
    std::atomic<int>    completed{0};
    std::atomic<bool>   gate{false};
    std::atomic<bool>   blocker_running{false};

    // First task blocks until we release the gate, occupying the only worker.
    q.submit([&](const CancellationToken& token) {
        blocker_running.store(true);
        while (!gate.load() && !token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        completed.fetch_add(1);
    });

    // Wait until the blocker is actually running on the worker thread —
    // otherwise drain_pending() could wipe it out before it was picked up.
    while (!blocker_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Queue a handful of pending tasks behind the running blocker.
    for (int i = 0; i < 5; ++i) {
        q.submit([&](const CancellationToken&) { completed.fetch_add(1); });
    }

    q.drain_pending();
    gate.store(true);

    // Allow time for the first (blocking) task to finish; drained tasks
    // should never run.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(completed.load(), 1) << "drained tasks were executed";
}

TEST(TaskQueueTest, Destructor_WhileTasksRunning)
{
    // Regression guard for the "destroy-while-busy" bug. The destructor
    // must ask the running task to cancel and join cleanly.
    std::atomic<bool> finished_cleanly{false};
    {
        TaskQueue         q(2);
        std::atomic<bool> started{false};

        q.submit([&](const CancellationToken& token) {
            started.store(true);
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            finished_cleanly.store(true);
        });

        // Wait until the task is actually running before destroying.
        while (!started.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } // ~TaskQueue() runs here — must not hang or crash.

    EXPECT_TRUE(finished_cleanly.load());
}

TEST(TaskQueueTest, MultipleWorkers_ParallelExecution)
{
    TaskQueue        q(4);
    std::atomic<int> running{0};
    std::atomic<int> peak_concurrency{0};
    std::atomic<int> done{0};

    for (int i = 0; i < 4; ++i) {
        q.submit([&](const CancellationToken&) {
            const int cur = running.fetch_add(1) + 1;
            int       prev_peak = peak_concurrency.load();
            while (cur > prev_peak &&
                   !peak_concurrency.compare_exchange_weak(prev_peak, cur)) {
                // retry
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            running.fetch_sub(1);
            done.fetch_add(1);
        });
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (done.load() < 4 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(done.load(), 4);
    EXPECT_GE(peak_concurrency.load(), 2) << "workers did not run in parallel";
}

} // namespace
