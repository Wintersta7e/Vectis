#include "platform/file_dialog.h"

#include <filesystem>
#include <string>
#include <string_view>

// This is the only translation unit that pulls in portable-file-dialogs.
// The header is >2k lines and uses `system()`/`popen()` under the hood
// on POSIX, which would trip clang-tidy (cert-env33-c, concurrency-mt-
// unsafe) if it were visible everywhere.
// NOLINTBEGIN(cert-env33-c,concurrency-mt-unsafe,misc-include-cleaner,cppcoreguidelines-pro-type-vararg)
#include <portable-file-dialogs.h>
// NOLINTEND(cert-env33-c,concurrency-mt-unsafe,misc-include-cleaner,cppcoreguidelines-pro-type-vararg)

namespace vectis::platform {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;

Result<std::filesystem::path> select_folder(std::string_view title)
{
    // pfd requires the title as `std::string` — copy the view.
    const std::string title_str{title};

    // pfd v0.1.0 has no programmatic backend-availability check: if no
    // backend is available (headless Linux without zenity/kdialog), the
    // picker silently returns an empty string. We treat both "user
    // cancelled" and "no backend" as the same caller-visible outcome.
    pfd::select_folder picker(title_str, std::string{});
    std::string        selection = picker.result();

    if (selection.empty()) {
        return make_error(
            ErrorKind::PlatformError,
            "folder selection was cancelled or no dialog backend available",
            "select_folder()");
    }

    return std::filesystem::path{std::move(selection)};
}

} // namespace vectis::platform
