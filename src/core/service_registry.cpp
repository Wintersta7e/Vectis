#include "core/service_registry.h"

#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>

#include "core/config_manager.h"
#include "core/context_bus.h"
#include "core/log.h"
#include "services/ai_engine/ai_engine.h"
#include "services/index_engine/index_engine.h"
#include "services/storage_engine/storage_engine.h"

namespace vectis::core {

/// Service ownership. `ai` is deferred (std::optional) because its
/// constructor reads config keys — it can only be built AFTER
/// `ConfigManager::load()` has populated them. `storage` and `index`
/// have no cross-service dependencies.
///
/// The registry no longer holds a shared HttpClient: each AI backend
/// owns its own, so the previous race (status-line probe racing with
/// an in-flight streaming request on the same `CURL*`) is gone by
/// construction. AskMode still owns its own HttpClient for WebSearch.
struct ServiceRegistry::Impl {
    ConfigManager                     config;
    ContextBus                        context;
    services::StorageEngine           storage;
    services::IndexEngine             index;
    std::optional<services::AIEngine> ai;
};

ServiceRegistry::ServiceRegistry() : m_impl(std::make_unique<Impl>()) {}
ServiceRegistry::~ServiceRegistry() = default;

ServiceRegistry::ServiceRegistry(ServiceRegistry&&) noexcept            = default;
ServiceRegistry& ServiceRegistry::operator=(ServiceRegistry&&) noexcept = default;

ConfigManager& ServiceRegistry::config()
{
    return m_impl->config;
}

ContextBus& ServiceRegistry::context()
{
    return m_impl->context;
}

services::AIEngine& ServiceRegistry::ai()
{
    if (!m_impl->ai.has_value()) {
        VECTIS_LOG_ERROR(
            "ServiceRegistry::ai() called before initialize_ai() — "
            "config-driven backend selection would be wrong");
        std::abort();
    }
    return *m_impl->ai;
}

void ServiceRegistry::initialize_ai()
{
    if (m_impl->ai.has_value()) return;
    m_impl->ai.emplace(m_impl->config);
}

services::IndexEngine& ServiceRegistry::index()
{
    return m_impl->index;
}

services::StorageEngine& ServiceRegistry::storage()
{
    return m_impl->storage;
}

} // namespace vectis::core
