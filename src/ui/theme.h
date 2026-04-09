#pragma once

#include <imgui.h>

/// Vectis visual theme.
///
/// Defines the dark-mode palette used across all modes and the entry
/// point (`apply_vectis_theme`) that stamps it into `ImGuiStyle`.
namespace vectis::ui {

/// Palette values are plain `ImVec4` RGBA (each component 0.0–1.0).
struct VectisColors {
    ImVec4 bg;
    ImVec4 panel;
    ImVec4 panel_alt;
    ImVec4 border;
    ImVec4 accent;
    ImVec4 accent_hover;
    ImVec4 accent_active;
    ImVec4 text;
    ImVec4 text_muted;
    ImVec4 text_disabled;
    ImVec4 error;
    ImVec4 warn;
    ImVec4 success;
};

/// Default dark palette. Hand-tuned to feel close to editor/terminal
/// dark themes without being pure black.
inline constexpr VectisColors k_dark_palette = {
    .bg            = {0.09F, 0.10F, 0.12F, 1.00F}, // #171A1F — app background
    .panel         = {0.12F, 0.13F, 0.16F, 1.00F}, // #1E2128 — panel background
    .panel_alt     = {0.15F, 0.17F, 0.20F, 1.00F}, // #262A33 — hover/alternate row
    .border        = {0.23F, 0.25F, 0.29F, 1.00F}, // #3B404A — subtle outlines
    .accent        = {0.27F, 0.54F, 0.92F, 1.00F}, // #458AEB — primary accent (buttons/focus)
    .accent_hover  = {0.35F, 0.63F, 0.98F, 1.00F}, // #59A0FA — hover accent
    .accent_active = {0.22F, 0.46F, 0.82F, 1.00F}, // #3876D1 — pressed accent
    .text          = {0.90F, 0.91F, 0.93F, 1.00F}, // #E6E8ED — primary text
    .text_muted    = {0.62F, 0.65F, 0.71F, 1.00F}, // #9EA6B5 — secondary labels
    .text_disabled = {0.40F, 0.42F, 0.46F, 1.00F}, // #666B76 — disabled widget text
    .error         = {0.93F, 0.33F, 0.35F, 1.00F}, // #EE555A — error text / icons
    .warn          = {0.96F, 0.73F, 0.30F, 1.00F}, // #F4BB4C — warning text / icons
    .success       = {0.45F, 0.82F, 0.52F, 1.00F}, // #72D184 — success text / icons
};

/// Apply the Vectis dark theme to an `ImGuiStyle` instance.
///
/// Assumes `ImGui::StyleColorsDark(&style)` has already been called so
/// all entries have baseline values; this overrides the ones that matter
/// for the Vectis look and tweaks spacing/rounding for consistency.
void apply_vectis_theme(ImGuiStyle& style);

} // namespace vectis::ui
