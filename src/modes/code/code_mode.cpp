#include "modes/code/code_mode.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/config_manager.h"
#include "core/context_bus.h"
#include "core/log.h"
#include "core/service_registry.h"
#include "core/task_queue.h"
#include "modes/code/code_index.h"
#include "modes/code/digest_exporter.h"
#include "modes/code/language.h"
#include "modes/code/parser.h"
#include "modes/code/code_index_store.h"
#include "modes/code/scanner.h"
#include "modes/code/symbol.h"
#include "platform/file_dialog.h"
#include "platform/file_io.h"
#include "services/index_engine/index_engine.h"
#include "services/storage_engine/storage_engine.h"

namespace vectis::modes::code {

namespace {

/// Hard cap on the number of bytes the code viewer will render.
/// InputTextMultiline's scroll performance degrades badly above ~1 MB
/// and the Step 2 viewer is deliberately minimal.
constexpr std::size_t k_max_viewable_bytes = 1ULL * 1024 * 1024;

/// Default exclude-dir names if the config file does not override.
const std::vector<std::string> k_default_excludes = {
    "node_modules", "dist", ".git", "vendor", "build", "target", "__pycache__", ".venv"
};

constexpr const char* k_panel_file_tree     = "File Tree";
constexpr const char* k_panel_code_viewer   = "Code Viewer";
constexpr const char* k_panel_symbol_browser= "Symbol Browser";
constexpr const char* k_panel_dependencies  = "Dependencies";
constexpr const char* k_dockspace_name      = "VectisDockspace";

} // namespace

CodeMode::CodeMode()
    : m_task_queue(std::make_unique<vectis::core::TaskQueue>(1)),
      m_index(std::make_unique<CodeIndex>())
{
}

CodeMode::~CodeMode() = default;

void CodeMode::initialize(vectis::core::ServiceRegistry& services)
{
    m_services = &services;
    m_config   = &services.config();
    m_bus      = &services.context();
    VECTIS_LOG_INFO("CodeMode initialized");
}

void CodeMode::shutdown()
{
    // Cancel any running scan so the task queue joins cleanly.
    m_scan_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (m_task_queue) {
        m_task_queue->cancel_all();
    }
    m_task_queue.reset();
    m_index.reset();
    VECTIS_LOG_INFO("CodeMode shut down");
}

// -----------------------------------------------------------------------------
// Actions
// -----------------------------------------------------------------------------

void CodeMode::on_open_folder_clicked()
{
    auto picked = vectis::platform::select_folder("Open Folder — Vectis");
    if (!picked) {
        VECTIS_LOG_INFO("Open Folder cancelled: {}", picked.error().message);
        return;
    }
    start_scan(*picked);
}

void CodeMode::on_export_digest_clicked(DigestFormat format)
{
    m_last_export_path.clear();
    m_last_export_error.clear();

    ExportOptions options;
    options.format       = format;
    options.project_root = m_project_root;
    options.project_name = m_project_root.filename().string();

    auto result = export_digest(*m_index, options);
    if (result) {
        m_last_export_path = result->string();
    } else {
        m_last_export_error = result.error().message;
        VECTIS_LOG_ERROR("Digest export failed: {}", m_last_export_error);
    }
}

bool CodeMode::try_load_cache(const std::filesystem::path& root)
{
    if (m_services == nullptr) return false;
    if (!has_cache_for(m_services->storage(), root)) return false;

    auto result = load_index(m_services->storage(), *m_index);
    if (!result) {
        VECTIS_LOG_WARN("CodeMode: cache load failed: {}", result.error().message);
        m_index->clear();
        return false;
    }

    VECTIS_LOG_INFO(
        "CodeMode: loaded from cache — {} files, {} symbols, {} deps",
        m_index->file_count(), m_index->symbol_count(), m_index->dependency_count());

    // Rebuild UI caches.
    m_cached_files = m_index->snapshot_files();
    m_tree_view.rebuild(m_cached_files);
    m_dep_view.rebuild(*m_index);
    refresh_filtered_symbols();

    // Publish the indexed event so other modes (future) can react.
    if (m_bus != nullptr) {
        ScanSummary summary;
        summary.file_count     = m_index->file_count();
        summary.symbol_count   = m_index->symbol_count();
        summary.language_count = m_index->language_count();
        m_bus->publish("codebase.indexed", vectis::core::ContextData{summary});
    }
    return true;
}

void CodeMode::persist_index(const std::filesystem::path& root)
{
    if (m_services == nullptr) return;

    CacheMetadata meta;
    meta.project_root   = root;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    meta.scan_timestamp = std::to_string(now);

    auto r = save_index(m_services->storage(), *m_index, meta);
    if (!r) {
        VECTIS_LOG_ERROR("CodeMode: persist_index failed: {}", r.error().message);
        return;
    }

    // Index file contents into FTS.
    auto& idx_engine = m_services->index();
    const auto files = m_index->snapshot_files();
    for (const auto& file : files) {
        const std::filesystem::path full = root / file.path_relative;
        auto content = vectis::platform::read_file(full);
        if (content) {
            idx_engine.index_file(file.id, file.path_relative.string(), *content);
        }
    }
    // Index symbols into FTS.
    for (const auto& file : files) {
        auto syms = m_index->symbols_in_file(file.id);
        if (!syms.empty()) {
            idx_engine.index_symbols(file.id, syms);
        }
    }
}

void CodeMode::start_scan(const std::filesystem::path& root)
{
    // Bump the epoch so any in-flight scan exits on its next batch boundary.
    const std::int64_t new_epoch = m_scan_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;

    m_index->clear();
    m_cached_files.clear();
    m_tree_view.clear();
    m_dep_view.clear();
    m_selected_file_id = 0;
    m_viewer_buffer.clear();
    m_viewer_buffer.push_back('\0');
    m_viewer_too_large = false;
    m_filtered_symbols.clear();
    m_symbol_filter.clear();
    m_project_root = root;

    {
        const std::scoped_lock lock(m_progress_mutex);
        m_progress_scanned_files = 0;
        m_progress_current_path.clear();
    }

    // Try loading from cache first.
    if (try_load_cache(root)) {
        return; // Incremental scan added in commit 8.
    }

    // Load exclude list from config (or defaults).
    std::vector<std::string> excludes_vec =
        m_config->get_string_array("code.exclude", k_default_excludes);
    std::unordered_set<std::string> excludes;
    excludes.reserve(excludes_vec.size());
    for (auto& name : excludes_vec) {
        excludes.insert(std::move(name));
    }

    // Kick off the scan task. The worker thread owns its own parser
    // because TreeSitterParser is not thread-safe.
    m_scan_running.store(true, std::memory_order_release);

    // We can't capture `*this` safely because CodeMode could be
    // destroyed while the task is running. But the task is cancelled
    // in shutdown() before m_task_queue is reset, so by contract the
    // pointers here outlive the task.
    CodeIndex*                      index_ptr    = m_index.get();
    vectis::core::ContextBus*       bus_ptr      = m_bus;
    vectis::core::ServiceRegistry*  services_ptr = m_services;
    auto                            epoch_ptr    = &m_scan_epoch;
    auto                            progress_mutex_ptr = &m_progress_mutex;
    auto                            progress_count_ptr = &m_progress_scanned_files;
    auto                            progress_path_ptr  = &m_progress_current_path;
    auto                            running_ptr        = &m_scan_running;

    ScanConfig cfg;
    cfg.root              = root;
    cfg.exclude_dir_names = std::move(excludes);
    cfg.epoch             = new_epoch;

    m_task_queue->submit([cfg,
                          index_ptr,
                          bus_ptr,
                          services_ptr,
                          epoch_ptr,
                          progress_mutex_ptr,
                          progress_count_ptr,
                          progress_path_ptr,
                          running_ptr](const vectis::core::CancellationToken& token) {
        TreeSitterParser parser;
        parser.register_builtin_languages();

        const auto on_progress = [&](const ScanProgress& progress) {
            {
                const std::scoped_lock lock(*progress_mutex_ptr);
                *progress_count_ptr = progress.files_scanned;
                *progress_path_ptr  = progress.current_path;
            }
            if (bus_ptr != nullptr) {
                bus_ptr->publish("codebase.scan.progress", vectis::core::ContextData{progress});
            }
        };

        const auto on_complete = [&](const ScanSummary& summary) {
            if (bus_ptr != nullptr) {
                bus_ptr->publish("codebase.indexed", vectis::core::ContextData{summary});
            }
        };

        const auto result = Scanner::run(
            cfg, *index_ptr, parser, on_progress, on_complete, token, *epoch_ptr);
        if (!result) {
            const auto& err = result.error();
            if (err.kind == vectis::core::ErrorKind::Cancelled) {
                VECTIS_LOG_INFO("Scan did not complete: {}", err.message);
            } else {
                VECTIS_LOG_ERROR(
                    "Scan failed: [{}] {}",
                    vectis::core::error_kind_to_string(err.kind),
                    err.message);
            }
        } else if (services_ptr != nullptr) {
            // Persist the index to SQLite and index into FTS.
            CacheMetadata meta;
            meta.project_root   = cfg.root;
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            meta.scan_timestamp = std::to_string(now);

            auto save_r = save_index(services_ptr->storage(), *index_ptr, meta);
            if (!save_r) {
                VECTIS_LOG_ERROR("Persist after scan failed: {}", save_r.error().message);
            } else {
                // Index file contents into FTS.
                const auto files = index_ptr->snapshot_files();
                for (const auto& file : files) {
                    const std::filesystem::path full = cfg.root / file.path_relative;
                    auto content = vectis::platform::read_file(full);
                    if (content) {
                        services_ptr->index().index_file(
                            file.id, file.path_relative.string(), *content);
                    }
                }
                // Index symbols into FTS.
                for (const auto& file : files) {
                    auto syms = index_ptr->symbols_in_file(file.id);
                    if (!syms.empty()) {
                        services_ptr->index().index_symbols(file.id, syms);
                    }
                }
            }
        }
        running_ptr->store(false, std::memory_order_release);
    });
}

void CodeMode::show_file(std::int64_t file_id)
{
    m_selected_file_id = file_id;
    m_viewer_buffer.clear();
    m_viewer_too_large = false;

    // Find the file entry in the cached snapshot.
    const auto it = std::find_if(
        m_cached_files.begin(), m_cached_files.end(),
        [file_id](const FileEntry& f) { return f.id == file_id; });
    if (it == m_cached_files.end()) {
        m_viewer_buffer.push_back('\0');
        return;
    }

    if (it->size > k_max_viewable_bytes) {
        m_viewer_too_large = true;
        m_viewer_buffer.push_back('\0');
        return;
    }

    const std::filesystem::path full = m_project_root / it->path_relative;
    auto contents = vectis::platform::read_file(full);
    if (!contents) {
        VECTIS_LOG_WARN(
            "CodeMode: failed to read '{}': {}",
            full.string(), contents.error().message);
        m_viewer_buffer.push_back('\0');
        return;
    }

    m_viewer_buffer.assign(contents->begin(), contents->end());
    m_viewer_buffer.push_back('\0');
}

void CodeMode::show_symbol(std::int64_t symbol_id)
{
    // Scroll to the matching symbol's line. For Step 2 we just jump
    // the viewer to that file — line-level scroll is a polish step.
    for (const Symbol& sym : m_filtered_symbols) {
        if (sym.id == symbol_id) {
            show_file(sym.file_id);
            return;
        }
    }
}

void CodeMode::refresh_filtered_symbols()
{
    m_filtered_symbols = m_index->search_symbols(m_symbol_filter);
}

// -----------------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------------

void CodeMode::ensure_docking_layout(ImGuiID dockspace_id)
{
    if (m_dock_layout_built) {
        return;
    }
    m_dock_layout_built = true;

    // If the user already has a saved layout (imgui.ini), respect it.
    if (ImGui::DockBuilderGetNode(dockspace_id) != nullptr &&
        ImGui::DockBuilderGetNode(dockspace_id)->IsSplitNode())
    {
        return;
    }

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

    ImGuiID main_id = dockspace_id;
    ImGuiID left_id = ImGui::DockBuilderSplitNode(
        main_id, ImGuiDir_Left, 0.22F, nullptr, &main_id);
    const ImGuiID right_id = ImGui::DockBuilderSplitNode(
        main_id, ImGuiDir_Right, 0.24F, nullptr, &main_id);
    // Split the center region horizontally so the Dependencies panel
    // lives below the code viewer, sharing the middle column.
    ImGuiID bottom_id = ImGui::DockBuilderSplitNode(
        main_id, ImGuiDir_Down, 0.32F, nullptr, &main_id);

    ImGui::DockBuilderDockWindow(k_panel_file_tree,      left_id);
    ImGui::DockBuilderDockWindow(k_panel_symbol_browser, right_id);
    ImGui::DockBuilderDockWindow(k_panel_code_viewer,    main_id);
    ImGui::DockBuilderDockWindow(k_panel_dependencies,   bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

void CodeMode::render()
{
    // Drain any completed-scan state that the worker thread left for us.
    // We check the epoch + running flag cheaply every frame; only when
    // the UI snapshot is stale do we grab the index lock.
    if (!m_scan_running.load(std::memory_order_acquire) &&
        m_index->file_count() != m_cached_files.size())
    {
        m_cached_files = m_index->snapshot_files();
        m_tree_view.rebuild(m_cached_files);
        m_dep_view.rebuild(*m_index);
        refresh_filtered_symbols();
    }

    // Set up the three-panel docking layout the first time this mode
    // renders (uses the dockspace id already established by App).
    const ImGuiID dockspace_id = ImGui::GetID(k_dockspace_name);
    ensure_docking_layout(dockspace_id);

    render_file_tree_panel();
    render_code_viewer_panel();
    render_symbol_browser_panel();
    render_dependencies_panel();
}

void CodeMode::render_file_tree_panel()
{
    if (!ImGui::Begin(k_panel_file_tree)) {
        ImGui::End();
        return;
    }

    // Header row: Open Folder + Export Digest + project root label.
    if (ImGui::Button("Open Folder...")) {
        on_open_folder_clicked();
    }
    ImGui::SameLine();

    const bool export_disabled = m_project_root.empty() ||
                                 m_index->file_count() == 0;
    ImGui::BeginDisabled(export_disabled);
    if (ImGui::Button("Export Digest...")) {
        ImGui::OpenPopup("##export_digest_popup");
    }
    ImGui::EndDisabled();

    if (ImGui::BeginPopup("##export_digest_popup")) {
        ImGui::TextDisabled("Choose format:");
        ImGui::Separator();
        if (ImGui::Button("JSON (full)", ImVec2(160.0F, 0.0F))) {
            on_export_digest_clicked(DigestFormat::Json);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Button("Markdown", ImVec2(160.0F, 0.0F))) {
            on_export_digest_clicked(DigestFormat::Markdown);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Button("Slim JSON", ImVec2(160.0F, 0.0F))) {
            on_export_digest_clicked(DigestFormat::SlimJson);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (m_project_root.empty()) {
        ImGui::TextDisabled("(no folder loaded)");
    } else {
        ImGui::TextUnformatted(m_project_root.filename().string().c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", m_project_root.string().c_str());
        }
    }

    // Last-export feedback line (success or error).
    if (!m_last_export_path.empty()) {
        ImGui::TextColored(
            ImVec4(0.45F, 0.82F, 0.52F, 1.0F),
            "exported: %s", m_last_export_path.c_str());
    } else if (!m_last_export_error.empty()) {
        ImGui::TextColored(
            ImVec4(0.93F, 0.33F, 0.35F, 1.0F),
            "export failed: %s", m_last_export_error.c_str());
    }

    ImGui::Separator();

    // Tree — wrapped in a child so the status line sticks to the bottom.
    const float status_h = ImGui::GetFrameHeightWithSpacing() + 4.0F;
    if (ImGui::BeginChild("##file_tree_scroll", ImVec2(0.0F, -status_h), false)) {
        m_tree_view.render(
            [this](std::int64_t file_id) { show_file(file_id); },
            m_selected_file_id);
    }
    ImGui::EndChild();

    ImGui::Separator();

    // Status line: "Indexed: N files, M symbols, K languages"
    // or     "Scanning: <path>..." while in progress.
    if (m_scan_running.load(std::memory_order_acquire)) {
        std::string current;
        std::size_t scanned = 0;
        {
            const std::scoped_lock lock(m_progress_mutex);
            current = m_progress_current_path;
            scanned = m_progress_scanned_files;
        }
        ImGui::Text("Scanning: %zu files...", scanned);
        if (!current.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", current.c_str());
        }
    } else {
        ImGui::Text(
            "Indexed: %zu files, %zu symbols, %zu languages",
            m_index->file_count(),
            m_index->symbol_count(),
            m_index->language_count());
    }

    ImGui::End();
}

void CodeMode::render_code_viewer_panel()
{
    if (!ImGui::Begin(k_panel_code_viewer)) {
        ImGui::End();
        return;
    }

    if (m_project_root.empty()) {
        // Empty state for the mode — no folder loaded at all.
        const char* headline = "Open a folder to begin";
        const char* body =
            "Click 'Open Folder...' in the File Tree panel to pick a\n"
            "project directory. Vectis will scan it in the background\n"
            "and populate the file tree and symbol browser.";
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 head_size = ImGui::CalcTextSize(headline);
        ImGui::SetCursorPos(ImVec2(
            (avail.x - head_size.x) * 0.5F,
            (avail.y * 0.35F)));
        ImGui::TextUnformatted(headline);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("%s", body);
    } else if (m_selected_file_id == 0) {
        ImGui::TextDisabled("(select a file in the File Tree)");
    } else if (m_viewer_too_large) {
        ImGui::TextDisabled("(file too large to display — > 1 MB)");
    } else {
        // InputTextMultiline wants a mutable char*; we back it with a
        // vector<char> ending in a trailing null.
        ImGui::InputTextMultiline(
            "##code_viewer_text",
            m_viewer_buffer.empty() ? nullptr : m_viewer_buffer.data(),
            m_viewer_buffer.empty() ? 0 : m_viewer_buffer.size(),
            ImVec2(-1.0F, -1.0F),
            ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo);
    }

    ImGui::End();
}

void CodeMode::render_dependencies_panel()
{
    if (!ImGui::Begin(k_panel_dependencies)) {
        ImGui::End();
        return;
    }

    m_dep_view.render(
        *m_index,
        [this](std::int64_t file_id) { show_file(file_id); },
        m_selected_file_id);

    ImGui::End();
}

void CodeMode::render_symbol_browser_panel()
{
    if (!ImGui::Begin(k_panel_symbol_browser)) {
        ImGui::End();
        return;
    }

    // Filter input at the top of the panel.
    char filter_buf[256];
    std::snprintf(filter_buf, sizeof(filter_buf), "%s", m_symbol_filter.c_str());
    if (ImGui::InputTextWithHint(
            "##symbol_filter", "filter symbols...",
            filter_buf, sizeof(filter_buf)))
    {
        m_symbol_filter.assign(filter_buf);
        refresh_filtered_symbols();
    }

    ImGui::Separator();

    if (ImGui::BeginChild("##symbol_list", ImVec2(0.0F, 0.0F), false)) {
        if (m_filtered_symbols.empty()) {
            ImGui::TextDisabled("(no symbols)");
        } else {
            for (const Symbol& sym : m_filtered_symbols) {
                const std::string label =
                    sym.name + "  [" + std::string(symbol_kind_name(sym.kind)) + "]";
                const bool is_selected = (sym.id == m_selected_file_id); // reuse for visual hint
                if (ImGui::Selectable(label.c_str(), is_selected)) {
                    show_symbol(sym.id);
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace vectis::modes::code
