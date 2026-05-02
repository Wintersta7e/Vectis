#include "platform/file_io.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <climits>

#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace vectis::platform {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;

namespace {

constexpr const char* k_data_dir_name = "vectis-data";

#if defined(_WIN32)
Result<std::filesystem::path> executable_path_windows()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD size =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return make_error(ErrorKind::PlatformError, "GetModuleFileNameW failed",
                              "executable_path()");
        }
        if (size < buffer.size()) {
            return std::filesystem::path{std::wstring{buffer.data(), size}};
        }
        // Buffer was too small — ERROR_INSUFFICIENT_BUFFER. Grow and retry.
        buffer.resize(buffer.size() * 2);
    }
}
#endif

#if defined(__linux__)
Result<std::filesystem::path> executable_path_linux()
{
    std::error_code ec;
    auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return make_error(ErrorKind::PlatformError,
                          "readlink(/proc/self/exe) failed: " + ec.message(), "executable_path()");
    }
    return path;
}
#endif

#if defined(__APPLE__)
Result<std::filesystem::path> executable_path_macos()
{
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return make_error(ErrorKind::PlatformError, "_NSGetExecutablePath failed",
                          "executable_path()");
    }
    std::error_code ec;
    auto canonical = std::filesystem::canonical(std::filesystem::path{buffer.data()}, ec);
    if (ec) {
        return make_error(ErrorKind::PlatformError,
                          "canonical() failed on executable path: " + ec.message(),
                          "executable_path()");
    }
    return canonical;
}
#endif

} // namespace

Result<std::filesystem::path> executable_path()
{
#if defined(_WIN32)
    return executable_path_windows();
#elif defined(__linux__)
    return executable_path_linux();
#elif defined(__APPLE__)
    return executable_path_macos();
#else
    return make_error(ErrorKind::PlatformError,
                      "executable_path() not implemented for this platform", "executable_path()");
#endif
}

Result<std::filesystem::path> executable_dir()
{
    auto exe = executable_path();
    if (!exe) {
        return tl::unexpected<vectis::core::Error>{exe.error()};
    }
    return exe->parent_path();
}

Result<std::filesystem::path> default_data_dir()
{
    auto dir = executable_dir();
    if (!dir) {
        return tl::unexpected<vectis::core::Error>{dir.error()};
    }
    return *dir / k_data_dir_name;
}

Result<void> ensure_dir(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return make_error(ErrorKind::IoError, "failed to create directory: " + ec.message(),
                          path.string());
    }
    return {};
}

Result<std::string> read_file(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return make_error(ErrorKind::IoError, "failed to open file for reading", path.string());
    }

    stream.seekg(0, std::ios::end);
    const auto end_pos = stream.tellg();
    if (end_pos < 0) {
        return make_error(ErrorKind::IoError, "failed to determine file size", path.string());
    }
    stream.seekg(0, std::ios::beg);

    std::string contents;
    contents.resize(static_cast<std::size_t>(end_pos));
    if (!contents.empty()) {
        stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            return make_error(ErrorKind::IoError, "failed to read file contents", path.string());
        }
    }
    return contents;
}

Result<void> write_file(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return make_error(ErrorKind::IoError, "failed to open file for writing", path.string());
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream) {
        return make_error(ErrorKind::IoError, "failed to write file contents", path.string());
    }
    return {};
}

} // namespace vectis::platform
