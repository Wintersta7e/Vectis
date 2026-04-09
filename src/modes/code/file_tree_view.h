#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "modes/code/symbol.h"

namespace vectis::modes::code {

/// Hierarchical view of a flat `FileEntry` list, built once per scan
/// and rendered every frame via `TreeNode`/`Selectable` widgets.
///
/// Building the tree incrementally per frame would stutter on large
/// codebases; instead, the caller rebuilds the structure whenever the
/// underlying `CodeIndex` publishes `codebase.indexed`.
class FileTreeView {
public:
    /// Callback fired when the user clicks a file row. Receives the
    /// clicked file's `FileEntry::id` so the caller can look up the
    /// full entry and show its content.
    using OnFileSelected = std::function<void(std::int64_t file_id)>;

    FileTreeView();
    ~FileTreeView();

    FileTreeView(const FileTreeView&)            = delete;
    FileTreeView& operator=(const FileTreeView&) = delete;

    /// Rebuild from a flat list of files. Safe to call repeatedly;
    /// replaces the previous structure entirely.
    void rebuild(const std::vector<FileEntry>& files);

    /// Clear all state (used when the user opens a new folder).
    void clear();

    /// Render the tree at the current ImGui cursor position.
    /// @param on_selected  Called with a file_id if the user clicks a file.
    /// @param selected_id  Currently selected file id, highlighted.
    void render(const OnFileSelected& on_selected, std::int64_t selected_id);

    /// Number of file entries in the tree. Useful for empty-state UX.
    [[nodiscard]] bool empty() const noexcept;

private:
    struct Node;
    std::unique_ptr<Node> m_root;

    void insert_file(const FileEntry& file);
};

} // namespace vectis::modes::code
