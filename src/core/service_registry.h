#pragma once

#include <memory>

namespace vectis::services {
class AIEngine;
class IndexEngine;
class StorageEngine;
} // namespace vectis::services

namespace vectis::core {

class ConfigManager;
class ContextBus;

/// Central access point for all shared services.
/// Passed to each mode during initialization.
class ServiceRegistry {
public:
    ServiceRegistry();
    ~ServiceRegistry();

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;
    ServiceRegistry(ServiceRegistry&&) noexcept;
    ServiceRegistry& operator=(ServiceRegistry&&) noexcept;

    services::AIEngine& ai();
    services::IndexEngine& index();
    services::StorageEngine& storage();
    ConfigManager& config();
    ContextBus& context();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::core
