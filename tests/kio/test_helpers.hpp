#pragma once

#include "kio/kio.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>

namespace kio::test {

// -----------------------------------------------------------------------------
// Run helpers
// -----------------------------------------------------------------------------

/// Run a task to completion with a timeout (throws on timeout)
template <typename T>
T RunWithTimeout(IoContext& ctx, Task<T>& t, std::chrono::milliseconds timeout) {
    t.start();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    ctx.Run([&] {
        if (t.done() || std::chrono::steady_clock::now() >= deadline) {
            ctx.Stop();
        }
    });
    if (!t.done()) {
        throw std::runtime_error("kio task timed out");
    }
    return t.result();
}

/// Run a task synchronously (no timeout, use for fast tests)
template <typename T>
T RunSync(Task<T> task) {
    IoContext ctx;
    ctx.RunUntilDone(task);
    return task.result();
}

inline void RunSync(Task<> task) {
    IoContext ctx;
    ctx.RunUntilDone(std::move(task));
    task.Result();
}

// -----------------------------------------------------------------------------
// File descriptor utilities
// -----------------------------------------------------------------------------

/// RAII guard for file descriptors
class FdGuard {
public:
    explicit FdGuard(int fd = -1) : fd_(fd) {}
    ~FdGuard() { Close(); }

    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;

    FdGuard(FdGuard&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    FdGuard& operator=(FdGuard&& other) noexcept {
        if (this != &other) {
            Close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int Get() const { return fd_; }
    [[nodiscard]] int Release() { return std::exchange(fd_, -1); }
    [[nodiscard]] bool Valid() const { return fd_ >= 0; }

    void Close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_;
};

/// Creates a temporary file (unlinked, so it disappears on close)
inline FdGuard MakeTempFile() {
    char path[] = "/tmp/aio_testXXXXXX";
    int fd = ::mkstemp(path);
    if (fd >= 0) {
        ::unlink(path);
    }
    return FdGuard{fd};
}

/// Creates a temporary file with initial content
inline FdGuard MakeTempFileWithContent(std::string_view content) {
    char path[] = "/tmp/aio_testXXXXXX";
    int fd = ::mkstemp(path);
    if (fd >= 0) {
        ::unlink(path);
        if (!content.empty()) {
            [[maybe_unused]] auto written = ::write(fd, content.data(), content.size());
            ::lseek(fd, 0, SEEK_SET);
        }
    }
    return FdGuard{fd};
}

/// Creates a temporary file with specific size (for sendfile tests)
inline FdGuard MakeTempFileWithSize(size_t size, std::byte fill = std::byte{0xAB}) {
    char path[] = "/tmp/aio_testXXXXXX";
    int fd = ::mkstemp(path);
    if (fd >= 0) {
        ::unlink(path);
        std::array<std::byte, 4096> buffer;
        buffer.fill(fill);
        size_t remaining = size;
        while (remaining > 0) {
            size_t chunk = std::min(remaining, buffer.size());
            [[maybe_unused]] auto written = ::write(fd, buffer.data(), chunk);
            remaining -= chunk;
        }
        ::lseek(fd, 0, SEEK_SET);
    }
    return FdGuard{fd};
}

/// Get file size
inline off_t GetFileSize(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) return -1;
    return st.st_size;
}

/// Creates a temporary directory path and removes it recursively on destruction.
class TempDir {
public:
    explicit TempDir(std::filesystem::path path = {}) : path_(std::move(path)) {}
    ~TempDir() {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    TempDir(TempDir&& other) noexcept : path_(std::move(other.path_)) {}
    TempDir& operator=(TempDir&& other) noexcept {
        if (this != &other) {
            std::error_code ec;
            if (!path_.empty()) {
                std::filesystem::remove_all(path_, ec);
            }
            path_ = std::move(other.path_);
        }
        return *this;
    }

    [[nodiscard]] const std::filesystem::path& Path() const { return path_; }
    [[nodiscard]] bool Valid() const { return !path_.empty(); }

private:
    std::filesystem::path path_;
};

inline TempDir MakeTempDir() {
    auto base = std::filesystem::temp_directory_path();
    std::string tmpl = (base / "kio_test_dir_XXXXXX").string();
    std::vector<char> buffer(tmpl.begin(), tmpl.end());
    buffer.push_back('\0');

    auto* dir = ::mkdtemp(buffer.data());
    if (dir == nullptr) {
        return TempDir{};
    }
    return TempDir{std::filesystem::path(dir)};
}

// -----------------------------------------------------------------------------
// Socket utilities
// -----------------------------------------------------------------------------

/// Socket pair for testing (RAII managed)
struct SocketPair {
    FdGuard fd1;
    FdGuard fd2;

    [[nodiscard]] bool Valid() const { return fd1.Valid() && fd2.Valid(); }
};

/// Creates a connected socket pair (AF_UNIX, SOCK_STREAM)
inline SocketPair MakeSocketPair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return {FdGuard{-1}, FdGuard{-1}};
    }
    return SocketPair{FdGuard{fds[0]}, FdGuard{fds[1]}};
}

/// Creates a non-blocking socket pair
inline SocketPair MakeNonBlockingSocketPair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) != 0) {
        return {FdGuard{-1}, FdGuard{-1}};
    }
    return {FdGuard{fds[0]}, FdGuard{fds[1]}};
}

/// Creates a datagram socket pair (for testing UDP-like behavior)
inline SocketPair MakeDatagramSocketPair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0) {
        return {FdGuard{-1}, FdGuard{-1}};
    }
    return {FdGuard{fds[0]}, FdGuard{fds[1]}};
}

// -----------------------------------------------------------------------------
// Data comparison utilities
// -----------------------------------------------------------------------------

/// Compare spans of bytes
inline bool SpansEqual(std::span<const std::byte> a, std::span<const std::byte> b) {
    if (a.size() != b.size()) return false;
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

/// Convert string to byte span
inline std::span<const std::byte> AsBytes(std::string_view sv) {
    return {reinterpret_cast<const std::byte*>(sv.data()), sv.size()};
}

/// Convert byte span to string
inline std::string AsString(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// -----------------------------------------------------------------------------
// Test patterns
// -----------------------------------------------------------------------------

/// Generate deterministic test data
inline std::vector<std::byte> GenerateTestData(size_t size, uint8_t seed = 0) {
    std::vector<std::byte> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = std::byte{static_cast<uint8_t>((i + seed) & 0xFF)};
    }
    return data;
}

/// Verify test data matches expected pattern
inline bool VerifyTestData(std::span<const std::byte> data, uint8_t seed = 0) {
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] != std::byte{static_cast<uint8_t>((i + seed) & 0xFF)}) {
            return false;
        }
    }
    return true;
}

}  // namespace kio::test
