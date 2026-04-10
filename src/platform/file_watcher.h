#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

#include "core/result.h"

namespace vectis::platform {

/// Type of file-system change detected by the watcher.
enum class FileChangeType : std::uint8_t {
    Created,
    Modified,
    Deleted,
};

/// Watches a directory tree for file changes.
///
/// Linux implementation uses `inotify` with a non-blocking fd.
/// Other platforms get a no-op stub that compiles but never fires.
///
/// The watcher is polled from the main thread each frame via `poll()`,
/// which dispatches queued events through the user-provided callback.
class FileWatcher {
public:
    using Callback = std::function<void(const std::filesystem::path& relative_path,
                                        FileChangeType type)>;

    FileWatcher();
    ~FileWatcher();

    FileWatcher(const FileWatcher&)            = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    FileWatcher(FileWatcher&&) noexcept;
    FileWatcher& operator=(FileWatcher&&) noexcept;

    /// Begin watching `root` recursively. Callback is invoked from
    /// `poll()` on the main thread with paths relative to `root`.
    [[nodiscard]] vectis::core::Result<void>
    watch(const std::filesystem::path& root, Callback on_change);

    /// Stop watching and release all OS resources.
    void stop();

    /// Dispatch any pending events. Call once per frame from the main
    /// thread. Non-blocking; returns immediately if no events are queued.
    void poll();

    /// Whether the watcher is currently active.
    [[nodiscard]] bool is_watching() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::platform
