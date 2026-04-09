#include "ui/theme.h"

#include <imgui.h>

namespace vectis::ui {

void apply_vectis_theme(ImGuiStyle& style)
{
    const VectisColors& c = k_dark_palette;

    // Rounding / spacing — flatter look, consistent padding.
    style.WindowRounding    = 6.0F;
    style.FrameRounding     = 4.0F;
    style.PopupRounding     = 4.0F;
    style.ScrollbarRounding = 4.0F;
    style.GrabRounding      = 4.0F;
    style.TabRounding       = 4.0F;
    style.ChildRounding     = 4.0F;

    style.WindowPadding     = ImVec2(12.0F, 10.0F);
    style.FramePadding      = ImVec2(8.0F, 5.0F);
    style.ItemSpacing       = ImVec2(8.0F, 6.0F);
    style.ItemInnerSpacing  = ImVec2(6.0F, 4.0F);
    style.IndentSpacing     = 18.0F;
    style.ScrollbarSize     = 12.0F;
    style.GrabMinSize       = 10.0F;

    style.WindowBorderSize  = 1.0F;
    style.FrameBorderSize   = 0.0F;
    style.PopupBorderSize   = 1.0F;
    style.TabBorderSize     = 0.0F;

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text]                  = c.text;
    colors[ImGuiCol_TextDisabled]          = c.text_disabled;

    colors[ImGuiCol_WindowBg]              = c.bg;
    colors[ImGuiCol_ChildBg]               = c.panel;
    colors[ImGuiCol_PopupBg]               = c.panel;
    colors[ImGuiCol_Border]                = c.border;
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);

    colors[ImGuiCol_FrameBg]               = c.panel;
    colors[ImGuiCol_FrameBgHovered]        = c.panel_alt;
    colors[ImGuiCol_FrameBgActive]         = c.accent_active;

    colors[ImGuiCol_TitleBg]               = c.panel;
    colors[ImGuiCol_TitleBgActive]         = c.panel_alt;
    colors[ImGuiCol_TitleBgCollapsed]      = c.panel;

    colors[ImGuiCol_MenuBarBg]             = c.panel;

    colors[ImGuiCol_ScrollbarBg]           = c.bg;
    colors[ImGuiCol_ScrollbarGrab]         = c.border;
    colors[ImGuiCol_ScrollbarGrabHovered]  = c.accent;
    colors[ImGuiCol_ScrollbarGrabActive]   = c.accent_active;

    colors[ImGuiCol_CheckMark]             = c.accent;
    colors[ImGuiCol_SliderGrab]            = c.accent;
    colors[ImGuiCol_SliderGrabActive]      = c.accent_active;

    colors[ImGuiCol_Button]                = c.accent;
    colors[ImGuiCol_ButtonHovered]         = c.accent_hover;
    colors[ImGuiCol_ButtonActive]          = c.accent_active;

    colors[ImGuiCol_Header]                = c.panel_alt;
    colors[ImGuiCol_HeaderHovered]         = c.accent;
    colors[ImGuiCol_HeaderActive]          = c.accent_active;

    colors[ImGuiCol_Separator]             = c.border;
    colors[ImGuiCol_SeparatorHovered]      = c.accent;
    colors[ImGuiCol_SeparatorActive]       = c.accent_active;

    colors[ImGuiCol_ResizeGrip]            = c.border;
    colors[ImGuiCol_ResizeGripHovered]     = c.accent;
    colors[ImGuiCol_ResizeGripActive]      = c.accent_active;

    colors[ImGuiCol_Tab]                   = c.panel;
    colors[ImGuiCol_TabHovered]            = c.accent_hover;
    colors[ImGuiCol_TabActive]             = c.accent;
    colors[ImGuiCol_TabUnfocused]          = c.panel;
    colors[ImGuiCol_TabUnfocusedActive]    = c.panel_alt;

    colors[ImGuiCol_DockingPreview]        = ImVec4(
        c.accent.x, c.accent.y, c.accent.z, 0.35F);
    colors[ImGuiCol_DockingEmptyBg]        = c.bg;

    colors[ImGuiCol_PlotLines]             = c.accent;
    colors[ImGuiCol_PlotLinesHovered]      = c.accent_hover;
    colors[ImGuiCol_PlotHistogram]         = c.accent;
    colors[ImGuiCol_PlotHistogramHovered]  = c.accent_hover;

    colors[ImGuiCol_TableHeaderBg]         = c.panel_alt;
    colors[ImGuiCol_TableBorderStrong]     = c.border;
    colors[ImGuiCol_TableBorderLight]      = c.border;
    colors[ImGuiCol_TableRowBg]            = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_TableRowBgAlt]         = c.panel_alt;

    colors[ImGuiCol_TextSelectedBg]        = ImVec4(
        c.accent.x, c.accent.y, c.accent.z, 0.35F);
    colors[ImGuiCol_DragDropTarget]        = c.accent;
    colors[ImGuiCol_NavHighlight]          = c.accent;
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0F, 1.0F, 1.0F, 0.70F);
    colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.0F, 0.0F, 0.0F, 0.50F);
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.0F, 0.0F, 0.0F, 0.60F);
}

} // namespace vectis::ui
