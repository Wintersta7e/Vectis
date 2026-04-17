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

    /// Returns the AI engine. Aborts with a clear log message if
    /// `initialize_ai()` has not been called yet — the engine needs
    /// the loaded config to wire its backends correctly.
    services::AIEngine& ai();

    /// Construct the AI engine using the already-loaded config. Must
    /// be called AFTER `config().load()` and before any mode or UI
    /// code touches `ai()`. Calling a second time is a no-op.
    void initialize_ai();

    services::IndexEngine& index();
    services::StorageEngine& storage();
    ConfigManager& config();
    ContextBus& context();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::core
