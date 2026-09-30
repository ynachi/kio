#pragma once

#include <unistd.h>

#include <utility>

namespace URing {

class UniqueFd {
   public:
    using Closer = void (*)(int) noexcept;
    static void DefaultCloser(const int fd) noexcept {
        if (fd >= 0) ::close(fd);
    }
    static void NoopCloser(int) noexcept {}

   private:
    int fd_ = -1;
    Closer closer_ = DefaultCloser;

   public:
    UniqueFd() noexcept = default;

    explicit UniqueFd(const int fd, Closer closer = DefaultCloser) noexcept
        : fd_(fd), closer_(closer) {}

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          closer_(std::exchange(other.closer_, DefaultCloser)) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            Reset();
            fd_ = std::exchange(other.fd_, -1);
            closer_ = std::exchange(other.closer_, DefaultCloser);
        }
        return *this;
    }

    ~UniqueFd() { Reset(); }

    [[nodiscard]] int Get() const noexcept { return fd_; }

    [[nodiscard]] bool Valid() const noexcept { return fd_ >= 0; }

    void Reset(const int fd = -1, Closer closer = DefaultCloser) noexcept {
        if (fd_ == fd && closer_ == closer) return;
        if (fd_ >= 0 && closer_) closer_(fd_);
        fd_ = fd;
        closer_ = closer;
    }

    int Release() noexcept {
        closer_ = DefaultCloser;
        return std::exchange(fd_, -1);
    }
};

}  // namespace URing
