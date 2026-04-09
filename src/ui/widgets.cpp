#include "ui/widgets.h"

#include <cmath>
#include <string>
#include <string_view>

#include <imgui.h>
#include <imgui_internal.h>

#include "ui/theme.h"

namespace vectis::ui {

namespace {

constexpr float k_title_font_scale   = 2.0F;
constexpr float k_tagline_font_scale = 1.1F;
constexpr float k_button_width       = 220.0F;
constexpr float k_button_height      = 38.0F;
constexpr float k_button_spacing     = 10.0F;
constexpr float k_title_gap          = 16.0F;
constexpr float k_buttons_gap        = 28.0F;

/// Render a single disabled welcome-screen button with an attached
/// tooltip explaining why it isn't clickable yet. All three Step 1
/// buttons go through this helper so the visual style stays consistent.
void render_disabled_button(const char* label, const char* tooltip)
{
    ImGui::BeginDisabled(true);
    ImGui::Button(label, ImVec2(k_button_width, k_button_height));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tooltip);
    }
}

} // namespace

void render_welcome_screen(std::string_view version)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2         center   = viewport->GetWorkCenter();

    // Estimate content height for vertical centering.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float       line_h = ImGui::GetFontSize();
    const float       content_height =
        (line_h * k_title_font_scale) + k_title_gap +
        (line_h * k_tagline_font_scale) + k_buttons_gap +
        (k_button_height * 3.0F) + (k_button_spacing * 2.0F) +
        (style.WindowPadding.y * 2.0F);

    ImGui::SetNextWindowPos(ImVec2(center.x, center.y), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(420.0F, content_height), ImGuiCond_Always);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoFocusOnAppearing;

    if (ImGui::Begin("##vectis_welcome", nullptr, flags)) {
        // --- Title -----------------------------------------------------
        const char* title = "Vectis";
        ImGui::SetWindowFontScale(k_title_font_scale);
        const float title_w = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - title_w) * 0.5F);
        ImGui::TextColored(k_dark_palette.text, "%s", title);
        ImGui::SetWindowFontScale(1.0F);

        ImGui::Dummy(ImVec2(0.0F, k_title_gap * 0.25F));

        // --- Tagline ---------------------------------------------------
        const std::string tagline = std::string{"Portable Developer Intelligence  ·  "} +
                                    std::string{version};
        ImGui::SetWindowFontScale(k_tagline_font_scale);
        const float tagline_w = ImGui::CalcTextSize(tagline.c_str()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - tagline_w) * 0.5F);
        ImGui::TextColored(k_dark_palette.text_muted, "%s", tagline.c_str());
        ImGui::SetWindowFontScale(1.0F);

        ImGui::Dummy(ImVec2(0.0F, k_buttons_gap));

        // --- Action buttons (all disabled in Step 1) -------------------
        const float button_x = (ImGui::GetWindowSize().x - k_button_width) * 0.5F;

        ImGui::SetCursorPosX(button_x);
        render_disabled_button(
            "Open Folder",
            "Available once Code mode is implemented (Step 2)");

        ImGui::Dummy(ImVec2(0.0F, k_button_spacing));
        ImGui::SetCursorPosX(button_x);
        render_disabled_button(
            "Start Proxy",
            "Available once HTTP mode is implemented (Step 9)");

        ImGui::Dummy(ImVec2(0.0F, k_button_spacing));
        ImGui::SetCursorPosX(button_x);
        render_disabled_button(
            "Ask a Question",
            "Available once Ask mode is implemented (Step 6)");
    }
    ImGui::End();
}

void render_spinner(float radius, float thickness, ImU32 color)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (draw_list == nullptr) {
        return;
    }

    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const ImVec2 center  = ImVec2(pos.x + radius, pos.y + radius);
    const float  time    = static_cast<float>(ImGui::GetTime());
    constexpr int k_segments   = 30;
    constexpr float k_sweep_t  = 1.2F;
    constexpr float k_rotation = 2.0F;

    const float t0 = std::fmod(time * k_rotation, 6.28318F);
    const float t1 = t0 + (k_sweep_t * 6.28318F / k_segments) * k_segments * 0.6F;

    draw_list->PathClear();
    for (int i = 0; i <= k_segments; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(k_segments);
        const float a = t0 + (t1 - t0) * f;
        draw_list->PathLineTo(
            ImVec2(center.x + std::cos(a) * radius, center.y + std::sin(a) * radius));
    }
    draw_list->PathStroke(color, ImDrawFlags_None, thickness);

    // Advance the ImGui cursor so layout callers get correct sizing.
    ImGui::Dummy(ImVec2(radius * 2.0F, radius * 2.0F));
}

} // namespace vectis::ui
