#pragma once

#include <filesystem>
#include <string_view>

#include "core/result.h"

/// Native file-dialog helpers.
///
/// Backed by `portable-file-dialogs`, a single-header library that
/// spawns native pickers (zenity/kdialog on Linux, IFileDialog on
/// Windows, NSOpenPanel on macOS). This header intentionally exposes
/// only a thin surface so the chunky pfd header is included in just
/// one translation unit (`file_dialog.cpp`).
///
/// Behaviour notes:
/// - On headless WSL2 or X-forwarded Linux without GTK, pfd silently
///   returns an empty selection. We translate that into a
///   `Result` error so callers can log and stay in an empty state
///   instead of crashing.
/// - Dialogs block the calling thread until the user dismisses them.
///   Always call from the UI thread.
namespace vectis::platform {

/// Show a native "pick a folder" dialog and return the selected path.
///
/// @param title  Window title shown in the dialog.
/// @return       Selected absolute path on success; `PlatformError`
///               if the user cancelled or no backend is available.
[[nodiscard]] vectis::core::Result<std::filesystem::path>
select_folder(std::string_view title);

} // namespace vectis::platform
