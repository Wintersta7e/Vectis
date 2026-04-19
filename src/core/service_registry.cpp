#include "core/service_registry.h"

#include <memory>
#include <utility>

#include "core/config_manager.h"
#include "core/context_bus.h"
#include "services/index_engine/index_engine.h"
#include "services/storage_engine/storage_engine.h"

namespace vectis::core {

struct ServiceRegistry::Impl {
    ConfigManager            config;
    ContextBus               context;
    services::StorageEngine  storage;
    services::IndexEngine    index;
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

services::IndexEngine& ServiceRegistry::index()
{
    return m_impl->index;
}

services::StorageEngine& ServiceRegistry::storage()
{
    return m_impl->storage;
}

} // namespace vectis::core
