#include "modes/code/dep_view.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "modes/code/code_index.h"
#include "modes/code/dependency.h"
#include "modes/code/dependency_graph.h"
#include "modes/code/symbol.h"

namespace vectis::modes::code {

namespace {

/// ASCII lowercase helper for the filter substring match.
[[nodiscard]] std::string to_lower_ascii(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (const char ch : input) {
        out.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

} // namespace

DependencyView::DependencyView()  = default;
DependencyView::~DependencyView() = default;

void DependencyView::clear()
{
    m_snapshot = DependencyViewSnapshot{};
    m_filter.clear();
}

void DependencyView::rebuild(const CodeIndex& index)
{
    DependencyViewSnapshot snap;

    // Pre-compute cycle membership once so the per-row flag is O(1).
    for (const DependencyCycle& cycle : detect_cycles(index)) {
        for (const std::int64_t id : cycle.file_ids) {
            snap.cycle_file_ids.insert(id);
        }
    }

    for (const FileEntry& file : index.snapshot_files()) {
        DependencyViewSnapshot::Row row;
        row.file_id        = file.id;
        row.relative_path  = file.path_relative.generic_string();
        row.outgoing_count = index.dependencies_of(file.id).size();
        row.incoming_count = index.dependents_of(file.id).size();
        row.in_cycle       = snap.cycle_file_ids.contains(file.id);
        snap.rows.push_back(std::move(row));
    }

    std::sort(snap.rows.begin(), snap.rows.end(),
              [](const auto& a, const auto& b) {
                  return a.relative_path < b.relative_path;
              });

    m_snapshot = std::move(snap);
}

bool DependencyView::empty() const noexcept
{
    return m_snapshot.rows.empty();
}

void DependencyView::render(const CodeIndex&        index,
                            const OnFileSelected&   on_file_selected,
                            std::int64_t            selected_file_id)
{
    // --- Header: summary + filter ----------------------------------
    {
        const std::size_t total_edges = index.dependency_count();
        const std::size_t cycle_count = m_snapshot.cycle_file_ids.empty()
            ? 0U
            : detect_cycles(index).size();
        ImGui::Text("Edges: %zu   Files in cycles: %zu   Cycles: %zu",
                    total_edges,
                    m_snapshot.cycle_file_ids.size(),
                    cycle_count);
    }

    char filter_buf[256];
    std::snprintf(filter_buf, sizeof(filter_buf), "%s", m_filter.c_str());
    if (ImGui::InputTextWithHint("##dep_filter", "filter files...",
                                 filter_buf, sizeof(filter_buf))) {
        m_filter.assign(filter_buf);
    }

    ImGui::Separator();

    if (m_snapshot.rows.empty()) {
        ImGui::TextDisabled(
            "(no dependencies — open a folder to populate the graph)");
        return;
    }

    // --- File list with fan-in / fan-out columns -------------------
    const std::string filter_lower = to_lower_ascii(m_filter);

    const float list_height =
        ImGui::GetContentRegionAvail().y * 0.55F;

    if (ImGui::BeginChild("##dep_list", ImVec2(0.0F, list_height), true)) {
        if (ImGui::BeginTable(
                "##dep_table", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("File",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Out",     ImGuiTableColumnFlags_WidthFixed, 40.0F);
            ImGui::TableSetupColumn("In",      ImGuiTableColumnFlags_WidthFixed, 40.0F);
            ImGui::TableSetupColumn("Cycle",   ImGuiTableColumnFlags_WidthFixed, 50.0F);
            ImGui::TableHeadersRow();

            for (const auto& row : m_snapshot.rows) {
                if (!filter_lower.empty()) {
                    const std::string lower_path = to_lower_ascii(row.relative_path);
                    if (lower_path.find(filter_lower) == std::string::npos) {
                        continue;
                    }
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool is_selected = (row.file_id == selected_file_id);
                if (ImGui::Selectable(row.relative_path.c_str(), is_selected,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    if (on_file_selected) {
                        on_file_selected(row.file_id);
                    }
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%zu", row.outgoing_count);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu", row.incoming_count);
                ImGui::TableSetColumnIndex(3);
                if (row.in_cycle) {
                    ImGui::TextColored(
                        ImVec4(0.93F, 0.33F, 0.35F, 1.0F), "cycle");
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::Separator();

    // --- Lower panel: outgoing + incoming for the selected file ----
    if (selected_file_id == 0) {
        ImGui::TextDisabled("(select a file above to see its direct edges)");
        return;
    }

    // Find the selected row for its name.
    std::string selected_name;
    for (const auto& row : m_snapshot.rows) {
        if (row.file_id == selected_file_id) {
            selected_name = row.relative_path;
            break;
        }
    }
    if (selected_name.empty()) {
        ImGui::TextDisabled("(selected file is not in the snapshot)");
        return;
    }

    ImGui::Text("Selected: %s", selected_name.c_str());

    if (ImGui::BeginChild("##dep_details", ImVec2(0.0F, 0.0F), false)) {
        if (ImGui::CollapsingHeader("Depends on (outgoing)",
                                    ImGuiTreeNodeFlags_DefaultOpen))
        {
            const auto outs = index.dependencies_of(selected_file_id);
            if (outs.empty()) {
                ImGui::TextDisabled("  (none)");
            } else {
                for (const auto& dep : outs) {
                    // Find the target row for display — externals are
                    // shown via their raw import string.
                    if (dep.target_file_id == 0) {
                        ImGui::BulletText("%s  [external]",
                                          dep.import_string.c_str());
                        continue;
                    }
                    std::string target_path;
                    for (const auto& row : m_snapshot.rows) {
                        if (row.file_id == dep.target_file_id) {
                            target_path = row.relative_path;
                            break;
                        }
                    }
                    if (target_path.empty()) {
                        target_path = "(unknown)";
                    }
                    ImGui::Bullet();
                    if (ImGui::Selectable(target_path.c_str())) {
                        if (on_file_selected) {
                            on_file_selected(dep.target_file_id);
                        }
                    }
                }
            }
        }

        if (ImGui::CollapsingHeader("Depended on by (incoming)",
                                    ImGuiTreeNodeFlags_DefaultOpen))
        {
            const auto ins = index.dependents_of(selected_file_id);
            if (ins.empty()) {
                ImGui::TextDisabled("  (none)");
            } else {
                for (const auto& dep : ins) {
                    std::string source_path;
                    for (const auto& row : m_snapshot.rows) {
                        if (row.file_id == dep.source_file_id) {
                            source_path = row.relative_path;
                            break;
                        }
                    }
                    if (source_path.empty()) {
                        source_path = "(unknown)";
                    }
                    ImGui::Bullet();
                    if (ImGui::Selectable(source_path.c_str())) {
                        if (on_file_selected) {
                            on_file_selected(dep.source_file_id);
                        }
                    }
                }
            }
        }
    }
    ImGui::EndChild();
}

} // namespace vectis::modes::code
