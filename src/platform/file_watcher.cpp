#include "platform/file_watcher.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "core/log.h"
#include "core/result.h"

#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace vectis::platform {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;
namespace fs = std::filesystem;

// ============================================================================
// Linux implementation (inotify)
// ============================================================================

#ifdef __linux__

struct FileWatcher::Impl
{
    int inotify_fd = -1;
    fs::path root;
    Callback callback;
    std::unordered_map<int, fs::path> wd_to_path; // watch descriptor → relative dir path
    bool watching = false;

    void add_watch_recursive(const fs::path& dir, const fs::path& relative)
    {
        const int wd =
            inotify_add_watch(inotify_fd, dir.c_str(),
                              IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);

        if (wd < 0) {
            if (errno == ENOSPC) {
                VECTIS_LOG_WARN("FileWatcher: inotify watch limit reached — "
                                "some subdirectories won't be watched. "
                                "Consider increasing fs.inotify.max_user_watches");
                return;
            }
            VECTIS_LOG_DEBUG("FileWatcher: inotify_add_watch failed for '{}': {}", dir.string(),
                             std::strerror(errno));
            return;
        }
        wd_to_path[wd] = relative;

        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            if (entry.is_directory(ec) && !ec) {
                const auto child_rel = relative / entry.path().filename();
                add_watch_recursive(entry.path(), child_rel);
            }
        }
    }
};

FileWatcher::FileWatcher() : m_impl(std::make_unique<Impl>()) {}

FileWatcher::~FileWatcher()
{
    stop();
}

FileWatcher::FileWatcher(FileWatcher&&) noexcept = default;
FileWatcher& FileWatcher::operator=(FileWatcher&&) noexcept = default;

Result<void> FileWatcher::watch(const fs::path& root, Callback on_change)
{
    stop();

    m_impl->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_impl->inotify_fd < 0) {
        return make_error(ErrorKind::PlatformError,
                          std::string("inotify_init1 failed: ") + std::strerror(errno));
    }

    m_impl->root = root;
    m_impl->callback = std::move(on_change);
    m_impl->add_watch_recursive(root, fs::path{});
    m_impl->watching = true;

    VECTIS_LOG_INFO("FileWatcher: watching '{}' ({} directories)", root.string(),
                    m_impl->wd_to_path.size());

    return {};
}

void FileWatcher::stop()
{
    if (m_impl->inotify_fd >= 0) {
        // Removing individual watches is unnecessary — closing the fd
        // releases all watches.
        ::close(m_impl->inotify_fd);
        m_impl->inotify_fd = -1;
    }
    m_impl->wd_to_path.clear();
    m_impl->watching = false;
}

void FileWatcher::poll()
{
    if (!m_impl->watching) {
        return;
    }

    // Read as many events as are available (non-blocking).
    alignas(inotify_event) std::array<char, 4096> buf{};
    while (true) {
        const auto len = ::read(m_impl->inotify_fd, buf.data(), buf.size());
        if (len <= 0) {
            break;
        }

        const char* ptr = buf.data();
        const char* end = buf.data() + len;
        while (ptr < end) {
            // inotify delivers a packed stream of variable-length
            // `inotify_event` records; the kernel ABI requires byte-pointer
            // reinterpretation to walk them.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto* event = reinterpret_cast<const inotify_event*>(ptr);

            if (event->len > 0 && m_impl->callback) {
                const auto wd_it = m_impl->wd_to_path.find(event->wd);
                if (wd_it != m_impl->wd_to_path.end()) {
                    const fs::path relative = wd_it->second / event->name;

                    if ((event->mask & IN_CREATE) != 0U) {
                        // If a new directory was created, start watching it.
                        if ((event->mask & IN_ISDIR) != 0U) {
                            const fs::path full = m_impl->root / relative;
                            m_impl->add_watch_recursive(full, relative);
                        }
                        else {
                            m_impl->callback(relative, FileChangeType::Created);
                        }
                    }
                    if ((event->mask & IN_CLOSE_WRITE) != 0U) {
                        m_impl->callback(relative, FileChangeType::Modified);
                    }
                    if ((event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0U) {
                        m_impl->callback(relative, FileChangeType::Deleted);
                    }
                    if ((event->mask & IN_MOVED_TO) != 0U) {
                        if ((event->mask & IN_ISDIR) != 0U) {
                            const fs::path full = m_impl->root / relative;
                            m_impl->add_watch_recursive(full, relative);
                        }
                        else {
                            m_impl->callback(relative, FileChangeType::Created);
                        }
                    }
                }
            }

            ptr += sizeof(inotify_event) + event->len;
        }
    }
}

bool FileWatcher::is_watching() const
{
    return m_impl->watching;
}

// ============================================================================
// Stub implementation (non-Linux platforms)
// ============================================================================

#else

struct FileWatcher::Impl
{
    bool watching = false;
};

FileWatcher::FileWatcher() : m_impl(std::make_unique<Impl>()) {}
FileWatcher::~FileWatcher() = default;

FileWatcher::FileWatcher(FileWatcher&&) noexcept = default;
FileWatcher& FileWatcher::operator=(FileWatcher&&) noexcept = default;

Result<void> FileWatcher::watch(const fs::path& /*root*/, Callback /*on_change*/)
{
    m_impl->watching = true;
    VECTIS_LOG_INFO("FileWatcher: stub (no-op on this platform)");
    return {};
}

void FileWatcher::stop()
{
    m_impl->watching = false;
}

void FileWatcher::poll()
{
    // no-op
}

bool FileWatcher::is_watching() const
{
    return m_impl->watching;
}

#endif

} // namespace vectis::platform
