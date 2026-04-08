#pragma once

#include <string_view>

namespace vectis::core {

class ServiceRegistry;

/// Base interface for all Vectis modes (Code, HTTP, Ask, Project, Write).
/// Each mode is an independent plugin that registers with the application core.
class IMode {
public:
    virtual ~IMode() = default;

    /// Unique identifier for this mode (e.g., "code", "http", "ask")
    virtual std::string_view id() const = 0;

    /// Human-readable display name (e.g., "Code", "HTTP", "Ask")
    virtual std::string_view name() const = 0;

    /// Called once at startup — mode should acquire service references here
    virtual void initialize(ServiceRegistry& services) = 0;

    /// Called every frame when this mode is active — render ImGui content here
    virtual void render() = 0;

    /// Called when mode is switched to foreground
    virtual void on_activate() = 0;

    /// Called when mode is switched to background
    virtual void on_deactivate() = 0;

    /// Called once at shutdown — release resources here
    virtual void shutdown() = 0;
};

} // namespace vectis::core
