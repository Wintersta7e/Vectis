#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

namespace vectis::core {

class TaskQueue; // forward declaration — `TaskQueue::submit` mints tokens.

/// Cooperative cancellation handle shared between a task caller and the
/// worker thread running the task.
///
/// The worker must poll `stop_requested()` periodically and bail out
/// when it returns true — this is cooperative, not forced.
/// `request_stop()` can be called from any thread.
///
/// Tokens are cheaply copyable (they hold a shared_ptr); copies of the
/// same token observe the same stop flag.
class CancellationToken {
public:
    /// True if any holder of this token has called `request_stop()`.
    [[nodiscard]] bool stop_requested() const noexcept
    {
        return m_flag->load(std::memory_order_acquire);
    }

    /// Flip the stop flag. Idempotent; safe from any thread.
    void request_stop() noexcept
    {
        m_flag->store(true, std::memory_order_release);
    }

private:
    friend class TaskQueue;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> flag)
        : m_flag(std::move(flag)) {}

    std::shared_ptr<std::atomic<bool>> m_flag;
};

/// Minimal N-worker background task runner.
///
/// Submitted tasks are queued FIFO and executed by any available worker
/// thread. The destructor sets a global stop flag, wakes workers, drops
/// any pending-but-not-yet-started tasks, and joins all worker threads.
/// In-progress tasks are asked to cancel cooperatively — they should
/// poll their `CancellationToken` to exit quickly.
///
/// Thread-safety:
/// - `submit`, `cancel_all`, `drain_pending` may be called from any
///   thread (including from inside a task callback, though that's
///   rarely useful).
/// - The destructor must NOT be called from inside a task.
class TaskQueue {
public:
    using Task = std::function<void(const CancellationToken&)>;

    /// @param worker_count  Number of worker threads. Clamped to >= 1.
    explicit TaskQueue(std::size_t worker_count = 1);
    ~TaskQueue();

    TaskQueue(const TaskQueue&)            = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;
    TaskQueue(TaskQueue&&)                 = delete;
    TaskQueue& operator=(TaskQueue&&)      = delete;

    /// Push a task onto the queue. The returned token can be used by
    /// the caller to cooperatively cancel this specific task.
    CancellationToken submit(Task task);

    /// Flip the stop flag on every pending and running task. Does not
    /// join workers and does not remove queued tasks from the queue —
    /// pending tasks will still run to give them a chance to observe
    /// the flag and exit fast.
    void cancel_all() noexcept;

    /// Remove and drop every pending (not-yet-started) task from the
    /// queue. Tasks already running are unaffected.
    void drain_pending() noexcept;

    /// Number of workers. Useful for tests and diagnostics.
    [[nodiscard]] std::size_t worker_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::core
