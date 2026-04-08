#pragma once

#include <any>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vectis::core {

/// Type-erased container for data published on the context bus.
using ContextData = std::any;

/// Callback signature for context bus subscribers.
using ContextCallback = std::function<void(const ContextData&)>;

/// Publish/subscribe bus for cross-mode communication.
/// Modes publish data (e.g., "codebase.indexed") and other modes
/// subscribe to react (e.g., Ask mode updates its search context).
class ContextBus {
public:
    /// Publish data to a named topic. All subscribers are notified.
    void publish(std::string_view topic, ContextData data);

    /// Subscribe to a named topic. Returns a subscription ID for later removal.
    uint64_t subscribe(std::string_view topic, ContextCallback callback);

    /// Remove a subscription by ID.
    void unsubscribe(uint64_t subscription_id);

private:
    struct Subscription {
        uint64_t id;
        ContextCallback callback;
    };

    std::mutex m_mutex;
    std::unordered_map<std::string, std::vector<Subscription>> m_subscribers;
    uint64_t m_next_id = 1;
};

} // namespace vectis::core
