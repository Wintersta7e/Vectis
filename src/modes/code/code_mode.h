#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/context_bus.h"
#include "core/mode.h"
#include "modes/code/code_index.h"
#include "platform/file_watcher.h"
#include "modes/code/dep_view.h"
#include "modes/code/digest_exporter.h"
#include "modes/code/file_tree_view.h"
#include "modes/code/parser.h"
#include "modes/code/symbol.h"

namespace vectis::core {
class ConfigManager;
class ServiceRegistry;
class TaskQueue;
} // namespace vectis::core

namespace vectis::modes::code {

/// Code mode — Vectis's first real IMode plugin.
///
/// Responsibilities:
/// - Render a three-panel layout (File Tree / Code Viewer / Symbol
///   Browser) inside the host dockspace.
/// - Handle the "Open Folder" action by invoking the native file
///   dialog and kicking off a background scan via `TaskQueue`.
/// - Marshal scan progress and completion events from the background
///   thread to the UI thread via a mutex-protected state block — no
///   ImGui calls ever happen on the worker thread.
class CodeMode final : public vectis::core::IMode {
public:
    CodeMode();
    ~CodeMode() override;

    CodeMode(const CodeMode&)            = delete;
    CodeMode& operator=(const CodeMode&) = delete;

    std::string_view id()   const override { return "code"; }
    std::string_view name() const override { return "Code"; }

    void initialize(vectis::core::ServiceRegistry& services) override;
    void render() override;
    // Rebuild the dock layout when returning to Code mode. The dockspace
    // node is shared with Ask mode, which will have overwritten it.
    void on_activate() override   { m_dock_layout_built = false; }
    void on_deactivate() override {}
    void shutdown() override;

private:
    // Actions
    void on_open_folder_clicked();
    void on_export_digest_clicked(DigestFormat format);
    void start_scan(const std::filesystem::path& root);
    void show_symbol(std::int64_t symbol_id);
    void show_file(std::int64_t file_id);

    // Rendering
    void render_file_tree_panel();
    void render_code_viewer_panel();
    void render_symbol_browser_panel();
    void render_dependencies_panel();
    void refresh_filtered_symbols();
    void ensure_docking_layout(ImGuiID dockspace_id);

    // Cache helpers
    bool try_load_cache(const std::filesystem::path& root);
    void persist_index(const std::filesystem::path& root);

    // Services (borrowed, owned by App)
    vectis::core::ServiceRegistry* m_services = nullptr;
    vectis::core::ConfigManager*   m_config   = nullptr;
    vectis::core::ContextBus*      m_bus      = nullptr;

    // Owned
    std::unique_ptr<vectis::core::TaskQueue>    m_task_queue;
    std::unique_ptr<CodeIndex>                  m_index;
    std::unique_ptr<vectis::platform::FileWatcher> m_watcher;

    // Scan state
    std::atomic<std::int64_t>                m_scan_epoch{0};
    std::filesystem::path                    m_project_root;
    std::atomic<bool>                        m_scan_running{false};

    // UI caches
    std::vector<FileEntry> m_cached_files;
    FileTreeView           m_tree_view;
    DependencyView         m_dep_view;
    std::int64_t           m_selected_file_id = 0;
    std::vector<char>      m_viewer_buffer;
    bool                   m_viewer_too_large = false;
    std::string            m_symbol_filter;
    std::vector<Symbol>    m_filtered_symbols;

    // Progress (written from worker thread, read by UI thread)
    std::mutex  m_progress_mutex;
    std::size_t m_progress_scanned_files = 0;
    std::string m_progress_current_path;

    // Last digest export outcome, shown briefly under the file tree.
    std::string m_last_export_path;
    std::string m_last_export_error;

    // Docking layout is rebuilt once on first render.
    bool          m_dock_layout_built = false;
    std::uint64_t m_last_generation   = 0;
};

} // namespace vectis::modes::code
