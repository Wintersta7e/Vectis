#pragma once

#include <string_view>

#include <imgui.h>

/// Reusable ImGui widgets shared across modes.
///
/// Step 1 provides only the pieces needed for the welcome screen and a
/// minimal loading spinner. Toasts and richer widgets are stubs that
/// will fill in as modes come online.
namespace vectis::ui {

/// Render the first-launch welcome screen in the center of the current
/// viewport. Shows the Vectis title, a tagline, and three placeholder
/// buttons (Open Folder, Start Proxy, Ask a Question) that are all
/// disabled in Step 1 and become interactive in later steps as the
/// corresponding modes are implemented.
void render_welcome_screen(std::string_view version);

/// Render a spinning loading indicator using the current draw list.
/// Not yet used by Step 1 code paths but kept here so modes can reach
/// for it without reinventing one.
///
/// @param radius    Spinner outer radius, in screen pixels.
/// @param thickness Stroke thickness, in screen pixels.
/// @param color     Line color (packed as ImU32).
void render_spinner(float radius, float thickness, ImU32 color);

} // namespace vectis::ui
