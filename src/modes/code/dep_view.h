#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace vectis::modes::code {

class CodeIndex;

/// Immutable view-model the UI panel renders — built once per scan
/// completion, not per frame, so a 10k-file project doesn't thrash.
struct DependencyViewSnapshot {
    /// Every file in the index with its outgoing + incoming counts
    /// pre-computed so the UI can sort and filter without touching
    /// the CodeIndex mutex.
    struct Row {
        std::int64_t file_id         = 0;
        std::string  relative_path;  ///< forward-slash, generic_string()
        std::size_t  outgoing_count  = 0;
        std::size_t  incoming_count  = 0;
        bool         in_cycle        = false;
    };

    std::vector<Row>                 rows;
    /// Union of all file_ids that participate in any detected cycle.
    /// The UI badges these rows with a warning color.
    std::unordered_set<std::int64_t> cycle_file_ids;
};

/// Render the Dependencies panel inside an active ImGui window.
///
/// The panel shows three vertically stacked sections:
///   1. A summary line: edge count, cycle count, filter input.
///   2. A table of files sorted by filename, with fan-out / fan-in
///      columns and a red badge for files in a cycle.
///   3. For the currently selected file, two collapsible sub-lists:
///      "Depends on" (outgoing) and "Depended on by" (incoming).
///
/// `on_file_selected` is invoked when the user clicks a row in the
/// dependency-of lists, so the Code mode can reveal that file in the
/// file tree + code viewer.
class DependencyView {
public:
    using OnFileSelected = std::function<void(std::int64_t)>;

    DependencyView();
    ~DependencyView();

    DependencyView(const DependencyView&)            = delete;
    DependencyView& operator=(const DependencyView&) = delete;

    /// Rebuild the snapshot from the current state of `index`. Call
    /// this once on scan completion (or whenever the file set or
    /// dependency graph has changed).
    void rebuild(const CodeIndex& index);

    /// Reset the view to its empty state.
    void clear();

    /// Render the panel. The caller owns the surrounding
    /// `ImGui::Begin/End` — `render` only draws content.
    void render(const CodeIndex&       index,
                const OnFileSelected&  on_file_selected,
                std::int64_t           currently_selected_file_id);

    [[nodiscard]] bool empty() const noexcept;

private:
    DependencyViewSnapshot m_snapshot;
    std::string            m_filter;
};

} // namespace vectis::modes::code
