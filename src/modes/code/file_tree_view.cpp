#include "modes/code/file_tree_view.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "modes/code/language.h"
#include "modes/code/symbol.h"

namespace vectis::modes::code {

/// One entry in the file tree. Non-leaf nodes have children and no
/// file payload; leaf nodes have a `file_id` and `Language`.
struct FileTreeView::Node {
    std::string                                      name;
    bool                                             is_file = false;
    std::int64_t                                     file_id = 0;
    Language                                         language = Language::Unknown;
    std::map<std::string, std::unique_ptr<Node>>     children;
};

FileTreeView::FileTreeView() : m_root(std::make_unique<Node>()) {}
FileTreeView::~FileTreeView() = default;

void FileTreeView::clear()
{
    m_root = std::make_unique<Node>();
}

void FileTreeView::rebuild(const std::vector<FileEntry>& files)
{
    clear();
    for (const FileEntry& file : files) {
        insert_file(file);
    }
}

void FileTreeView::insert_file(const FileEntry& file)
{
    Node* current = m_root.get();
    const auto& path = file.path_relative;

    // Walk intermediate directory segments, creating nodes on demand.
    auto it  = path.begin();
    auto end = path.end();
    if (it == end) {
        return;
    }

    // Everything before the filename is a directory segment.
    auto last = end;
    --last;  // safe because path has at least one segment

    for (; it != last; ++it) {
        const std::string segment = it->string();
        if (segment.empty() || segment == ".") {
            continue;
        }
        auto& child = current->children[segment];
        if (!child) {
            child       = std::make_unique<Node>();
            child->name = segment;
        }
        current = child.get();
    }

    const std::string filename = last->string();
    auto& leaf = current->children[filename];
    if (!leaf) {
        leaf       = std::make_unique<Node>();
        leaf->name = filename;
    }
    leaf->is_file  = true;
    leaf->file_id  = file.id;
    leaf->language = file.language;
}

void FileTreeView::render(const OnFileSelected& on_selected, std::int64_t selected_id)
{
    if (!m_root || m_root->children.empty()) {
        ImGui::TextDisabled("(no files indexed)");
        return;
    }

    // Depth-first render helper as a local recursive lambda so it can
    // reach the private `Node` type without any friending or forward
    // declarations leaking.
    const std::function<void(const Node*)> walk = [&](const Node* node) {
        for (const auto& [child_name, child] : node->children) {
            if (child->is_file) {
                // Colored dot before the filename.
                const ImVec4 color = language_color(child->language);
                ImGui::ColorButton(
                    ("##dot_" + std::to_string(child->file_id)).c_str(),
                    color,
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder |
                        ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoInputs,
                    ImVec2(8.0F, 8.0F));
                ImGui::SameLine();

                const bool is_selected = (child->file_id == selected_id);
                if (ImGui::Selectable(child->name.c_str(), is_selected)) {
                    if (on_selected) {
                        on_selected(child->file_id);
                    }
                }
            } else {
                const ImGuiTreeNodeFlags flags =
                    ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                    ImGuiTreeNodeFlags_SpanAvailWidth;
                if (ImGui::TreeNodeEx(child->name.c_str(), flags)) {
                    walk(child.get());
                    ImGui::TreePop();
                }
            }
        }
    };

    walk(m_root.get());
}

bool FileTreeView::empty() const noexcept
{
    return !m_root || m_root->children.empty();
}

} // namespace vectis::modes::code
