#pragma once

#include <filesystem>
#include <string>

namespace vectis::code {

/// Lexically normalise `absolute` and re-express it relative to
/// `root`, returning a forward-slash string suitable for the CodeIndex
/// path index. No filesystem access; safe for paths that don't exist
/// yet (e.g. cross-manifest references to a sibling whose file is
/// still being registered).
[[nodiscard]] inline std::string normalise_relative(const std::filesystem::path& absolute,
                                                    const std::filesystem::path& root)
{
    return absolute.lexically_normal().lexically_relative(root).generic_string();
}

} // namespace vectis::code
