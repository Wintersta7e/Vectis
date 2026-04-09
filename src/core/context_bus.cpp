#include "core/context_bus.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vectis::core {

void ContextBus::publish(std::string_view topic, ContextData data)
{
    // Copy the subscriber list under the lock, then release the lock
    // before invoking callbacks. This prevents re-entrant publish/
    // unsubscribe calls inside a callback from deadlocking on the
    // same mutex.
    std::vector<Subscription> targets;
    {
        const std::scoped_lock lock(m_mutex);
        auto                   it = m_subscribers.find(std::string{topic});
        if (it == m_subscribers.end()) {
            return;
        }
        targets = it->second;
    }

    for (const auto& sub : targets) {
        if (sub.callback) {
            sub.callback(data);
        }
    }
}

std::uint64_t ContextBus::subscribe(std::string_view topic, ContextCallback callback)
{
    const std::scoped_lock lock(m_mutex);
    const std::uint64_t    id = m_next_id++;
    m_subscribers[std::string{topic}].push_back(Subscription{id, std::move(callback)});
    return id;
}

void ContextBus::unsubscribe(std::uint64_t subscription_id)
{
    const std::scoped_lock lock(m_mutex);
    for (auto& [topic, subs] : m_subscribers) {
        const auto new_end = std::remove_if(
            subs.begin(), subs.end(),
            [subscription_id](const Subscription& s) { return s.id == subscription_id; });
        if (new_end != subs.end()) {
            subs.erase(new_end, subs.end());
            return;
        }
    }
}

} // namespace vectis::core
