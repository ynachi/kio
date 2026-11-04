#pragma once

// Shared test utilities. Everything here is test-side only: the suite is
// deliberately black-box (public API + death tests on CHECK messages), so
// there are no test hooks in production code to lean on.
//
// Conventions used across the suite:
//   - EXPECT (not ASSERT) inside coroutine bodies: ASSERT_* expands to a
//     plain `return`, which is ill-formed in a coroutine.
//   - Death tests spawn/drive INSIDE the EXPECT_DEATH statement so only the
//     forked child owns never-run work (a parent ~Context would CHECK-abort
//     on spawned-but-never-run tasks).
//   - Rings are tiny (entries=64, batch_max_size=8) unless a test is
//     specifically about queue depth.

#include "kio/context.h"

#include <coroutine>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <dirent.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace kio::test
{
/// Number of fds this process currently holds. The count includes
/// readdir()'s own handle plus "." and "..", so exactly three entries
/// are subtracted; the leak tests only compare before/after deltas.
inline std::size_t OpenFdCount() noexcept
{
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr)
    {
        return 0;
    }
    std::size_t n = 0;
    while (::readdir(dir) != nullptr)
    {
        ++n;
    }
    ::closedir(dir);
    return n > 3 ? n - 3 : 0;
}

/// RAII scratch directory for file-operation tests.
class TempDir
{
public:
    TempDir()
    {
        std::string tpl = (std::filesystem::temp_directory_path() / "kio_test_XXXXXX").string();
        std::vector<char> buf(tpl.begin(), tpl.end());
        buf.push_back('\0');
        if (const char* made = ::mkdtemp(buf.data()))
        {
            path_ = made;
        }
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// Tiny ring + small epoch cap, the default shape for uring tests.
inline io::IoOptions test_opts(const std::uint32_t entries = 64, const std::size_t batch_max_size = 8)
{
    io::IoOptions opts;
    opts.entries = entries;
    opts.batch_max_size = batch_max_size;
    return opts;
}

/// getsockopt readback helper for the option tests.
inline int read_int_option(const int fd, const int level, const int name)
{
    int v = -1;
    socklen_t len = sizeof(v);
    EXPECT_EQ(::getsockopt(fd, level, name, &v, &len), 0);
    return v;
}
}  // namespace kio::test
