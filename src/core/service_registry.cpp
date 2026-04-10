#include "core/service_registry.h"

#include <cstdlib>
#include <memory>
#include <utility>

#include "core/config_manager.h"
#include "core/context_bus.h"
#include "core/log.h"
#include "services/storage_engine/storage_engine.h"

namespace vectis::core {

struct ServiceRegistry::Impl {
    ConfigManager              config;
    ContextBus                 context;
    services::StorageEngine    storage;
};

namespace {

/// Abort with a clear log message when code reaches a service accessor
/// whose backing implementation has not yet been wired up in the current
/// build step. This is a programmer error, not a runtime condition, so we
/// fail loudly rather than returning a null reference or throwing.
[[noreturn]] void abort_unwired(const char* service_name)
{
    VECTIS_LOG_ERROR(
        "ServiceRegistry::{}() called but service is not yet available in this build step",
        service_name);
    std::abort();
}

} // namespace

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
    abort_unwired("ai");
}

services::IndexEngine& ServiceRegistry::index()
{
    abort_unwired("index");
}

services::StorageEngine& ServiceRegistry::storage()
{
    return m_impl->storage;
}

} // namespace vectis::core
