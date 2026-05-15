#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace vectis::code::cmake {

/// Coarse classification of what the *root* `CMakeLists.txt` builds.
/// Scoped to the root file deliberately: real CMake projects fan their
/// targets out through `add_subdirectory(...)` calls (most notably
/// tests in `tests/CMakeLists.txt`), and a corroborator is more
/// trustworthy when it only consults the project-shape declaration
/// the user wrote at the top.
enum class RootTargetShape : std::uint8_t
{
    /// At least one `add_library(NAME ...)` (excluding ALIAS / IMPORTED
    /// forms) and zero `add_executable(NAME ...)`. Strongly corroborates
    /// the Library label for C/C++ projects.
    LibraryOnly,
    /// At least one `add_executable(NAME ...)` and zero project-defined
    /// `add_library`. Suggests the root is an application, not a
    /// library — left here for symmetry; the detector currently uses
    /// only the LibraryOnly value.
    ExecutableOnly,
    /// Both kinds present at the root. Common in projects that ship a
    /// library plus a sibling CLI tool; corroborates neither extreme.
    Mixed,
};

/// One token per shape, formatted for the `signals[]` array. Returned
/// view points at a static literal — valid for the program's lifetime.
[[nodiscard]] std::string_view signal_for(RootTargetShape shape) noexcept;

/// Parse the root `CMakeLists.txt` and classify the target shape.
/// Returns `nullopt` if the file is missing, unreadable, or declares
/// no `add_library` / `add_executable` calls at all. The parser is a
/// best-effort lexical scan — sufficient for the corroborator's
/// precision target but not a full CMake interpreter. Conditional
/// branches and `function`-wrapped calls are honoured if the
/// `add_*` call text appears in the file at all.
[[nodiscard]] std::optional<RootTargetShape>
inspect_root(const std::filesystem::path& project_root);

} // namespace vectis::code::cmake
