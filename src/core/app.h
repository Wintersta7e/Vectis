#pragma once

#include "mode.h"
#include "service_registry.h"

#include <memory>
#include <string_view>
#include <vector>

namespace vectis::core {

/// Main application class. Manages the window, mode lifecycle, and main loop.
class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// Register a mode plugin. Call before run().
    void register_mode(std::unique_ptr<IMode> mode);

    /// Switch to a mode by its ID.
    void switch_mode(std::string_view mode_id);

    /// Initialize SDL2, OpenGL, ImGui, and all registered modes.
    bool initialize();

    /// Enter the main loop. Returns exit code.
    int run();

    /// Request shutdown (e.g., from a menu item).
    void request_shutdown();

private:
    void render_frame();
    void render_mode_tabs();
    void render_welcome_screen();
    void shutdown_internal();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::core
