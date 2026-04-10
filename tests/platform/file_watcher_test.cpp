#include "platform/file_watcher.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;
using vectis::platform::FileChangeType;
using vectis::platform::FileWatcher;

#ifdef __linux__

struct Event {
    fs::path       path;
    FileChangeType type;
};

class FileWatcherTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        m_tmp_dir = fs::temp_directory_path() / "vectis_watcher_test";
        fs::create_directories(m_tmp_dir);
    }

    void TearDown() override
    {
        m_watcher.stop();
        std::error_code ec;
        fs::remove_all(m_tmp_dir, ec);
    }

    FileWatcher m_watcher;
    fs::path    m_tmp_dir;
    std::vector<Event> m_events;

    void start_watching()
    {
        auto r = m_watcher.watch(m_tmp_dir,
            [this](const fs::path& path, FileChangeType type) {
                m_events.push_back({path, type});
            });
        ASSERT_TRUE(r) << r.error().message;
    }

    /// Wait for at least `n` events or `timeout`, whichever first.
    void wait_for_events(std::size_t n,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds{1000})
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (m_events.size() < n && std::chrono::steady_clock::now() < deadline) {
            m_watcher.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        m_watcher.poll();
    }
};

TEST_F(FileWatcherTest, DetectsNewFile)
{
    start_watching();

    // Create a new file.
    {
        std::ofstream ofs(m_tmp_dir / "hello.txt");
        ofs << "hello world";
    }

    wait_for_events(1);

    // Should have at least a Created or Modified event.
    ASSERT_FALSE(m_events.empty());
    bool found = false;
    for (const auto& e : m_events) {
        if (e.path.filename() == "hello.txt") {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "Expected an event for hello.txt";
}

TEST_F(FileWatcherTest, DetectsModifiedFile)
{
    // Create file before watching.
    {
        std::ofstream ofs(m_tmp_dir / "existing.txt");
        ofs << "initial";
    }

    start_watching();

    // Modify the file.
    {
        std::ofstream ofs(m_tmp_dir / "existing.txt");
        ofs << "modified content";
    }

    wait_for_events(1);

    bool found_modify = false;
    for (const auto& e : m_events) {
        if (e.path.filename() == "existing.txt" &&
            e.type == FileChangeType::Modified)
        {
            found_modify = true;
        }
    }
    EXPECT_TRUE(found_modify) << "Expected a Modified event for existing.txt";
}

TEST_F(FileWatcherTest, DetectsDeletedFile)
{
    // Create file before watching.
    {
        std::ofstream ofs(m_tmp_dir / "doomed.txt");
        ofs << "bye";
    }

    start_watching();

    fs::remove(m_tmp_dir / "doomed.txt");

    wait_for_events(1);

    bool found_delete = false;
    for (const auto& e : m_events) {
        if (e.path.filename() == "doomed.txt" &&
            e.type == FileChangeType::Deleted)
        {
            found_delete = true;
        }
    }
    EXPECT_TRUE(found_delete) << "Expected a Deleted event for doomed.txt";
}

TEST_F(FileWatcherTest, StopCeasesFiring)
{
    start_watching();
    m_watcher.stop();

    EXPECT_FALSE(m_watcher.is_watching());

    // Create a file — no event should arrive.
    {
        std::ofstream ofs(m_tmp_dir / "ignored.txt");
        ofs << "should not trigger";
    }

    wait_for_events(1, std::chrono::milliseconds{200});

    EXPECT_TRUE(m_events.empty());
}

#else

// On non-Linux platforms, just verify the stub compiles and doesn't crash.
TEST(FileWatcherStubTest, WatchSucceeds)
{
    FileWatcher watcher;
    auto tmp = fs::temp_directory_path() / "vectis_watcher_stub";
    fs::create_directories(tmp);

    auto r = watcher.watch(tmp, [](const fs::path&, FileChangeType) {});
    EXPECT_TRUE(r);
    EXPECT_TRUE(watcher.is_watching());

    watcher.poll(); // no-op
    watcher.stop();
    EXPECT_FALSE(watcher.is_watching());

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

#endif

} // namespace
