#include "core/task_queue.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.h"

namespace vectis::core {

namespace {

/// One queued-or-running task together with its cancellation flag.
/// The flag is heap-allocated via shared_ptr so CancellationToken
/// copies share the same state.
struct TaskSlot
{
    TaskQueue::Task task;
    std::shared_ptr<std::atomic<bool>> cancel_flag;
};

} // namespace

struct TaskQueue::Impl
{
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<TaskSlot> queue;
    std::vector<std::thread> workers;

    /// Set when the TaskQueue is shutting down. Workers exit as soon
    /// as they notice this.
    std::atomic<bool> shutting_down{false};

    /// Every running task's cancel flag goes here so `cancel_all()`
    /// can flip them without holding the main mutex. Entries are
    /// removed when the task completes.
    std::mutex running_mutex;
    std::vector<std::shared_ptr<std::atomic<bool>>> running_flags;

    void worker_loop()
    {
        for (;;) {
            TaskSlot slot;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] {
                    return shutting_down.load(std::memory_order_acquire) || !queue.empty();
                });

                if (shutting_down.load(std::memory_order_acquire) && queue.empty()) {
                    return;
                }

                slot = std::move(queue.front());
                queue.pop_front();
            }

            // Register this task's cancel flag with the running list.
            {
                const std::scoped_lock lock(running_mutex);
                running_flags.push_back(slot.cancel_flag);
            }

            const CancellationToken token(slot.cancel_flag);
            try {
                slot.task(token);
            }
            catch (const std::exception& e) {
                VECTIS_LOG_ERROR("TaskQueue: task threw exception: {}", e.what());
            }
            catch (...) {
                VECTIS_LOG_ERROR("TaskQueue: task threw unknown exception");
            }

            // Deregister the cancel flag now that the task is done.
            {
                const std::scoped_lock lock(running_mutex);
                const auto it =
                    std::find(running_flags.begin(), running_flags.end(), slot.cancel_flag);
                if (it != running_flags.end()) {
                    running_flags.erase(it);
                }
            }
        }
    }
};

TaskQueue::TaskQueue(std::size_t worker_count) : m_impl(std::make_unique<Impl>())
{
    const std::size_t clamped = worker_count == 0 ? 1 : worker_count;
    m_impl->workers.reserve(clamped);
    for (std::size_t i = 0; i < clamped; ++i) {
        m_impl->workers.emplace_back([impl = m_impl.get()] { impl->worker_loop(); });
    }
}

TaskQueue::~TaskQueue()
{
    // Flip the shutdown flag so workers exit once the queue drains,
    // and also flip every running task's cancel flag to ask them to
    // exit early via cooperative cancellation.
    m_impl->shutting_down.store(true, std::memory_order_release);
    cancel_all();

    // Drop pending tasks so workers don't feel obliged to run them
    // during shutdown.
    drain_pending();

    m_impl->wake.notify_all();
    for (auto& worker : m_impl->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

CancellationToken TaskQueue::submit(Task task)
{
    auto flag = std::make_shared<std::atomic<bool>>(false);

    {
        const std::scoped_lock lock(m_impl->mutex);
        m_impl->queue.push_back(TaskSlot{std::move(task), flag});
    }
    m_impl->wake.notify_one();

    return CancellationToken{flag};
}

void TaskQueue::cancel_all() noexcept
{
    // Cancel running tasks first (they're the urgent ones).
    {
        const std::scoped_lock lock(m_impl->running_mutex);
        for (const auto& flag : m_impl->running_flags) {
            flag->store(true, std::memory_order_release);
        }
    }
    // Then cancel pending tasks. They'll still run (briefly) so they
    // can observe the flag and exit.
    {
        const std::scoped_lock lock(m_impl->mutex);
        for (auto& slot : m_impl->queue) {
            slot.cancel_flag->store(true, std::memory_order_release);
        }
    }
}

void TaskQueue::drain_pending() noexcept
{
    const std::scoped_lock lock(m_impl->mutex);
    m_impl->queue.clear();
}

std::size_t TaskQueue::worker_count() const noexcept
{
    return m_impl->workers.size();
}

} // namespace vectis::core
