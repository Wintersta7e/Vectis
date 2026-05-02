#pragma once

#include <memory>

namespace vectis::services {
class IndexEngine;
class StorageEngine;
} // namespace vectis::services

namespace vectis::core {

class ConfigManager;

/// Central access point for shared long-lived services. Owns each by
/// value through the Pimpl; consumers hold references returned by
/// the accessors below. The older cross-mode pub/sub (ContextBus)
/// was removed with the GUI on 2026-04-22 — the CLI uses direct
/// callbacks, which is sufficient for a single-consumer tool.
class ServiceRegistry
{
public:
    ServiceRegistry();
    ~ServiceRegistry();

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;
    ServiceRegistry(ServiceRegistry&&) noexcept;
    ServiceRegistry& operator=(ServiceRegistry&&) noexcept;

    services::IndexEngine& index();
    services::StorageEngine& storage();
    ConfigManager& config();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::core
