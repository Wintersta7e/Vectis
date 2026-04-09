#include "platform/process.h"

#include <cstdlib>
#include <string>

namespace vectis::platform {

std::optional<std::string> get_env(std::string_view name)
{
    // std::getenv requires a null-terminated C string; copy the view.
    const std::string name_str{name};
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — see header comment.
    const char* raw = std::getenv(name_str.c_str());
    if (raw == nullptr) {
        return std::nullopt;
    }
    std::string value{raw};
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

} // namespace vectis::platform
