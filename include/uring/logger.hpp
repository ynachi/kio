#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <mutex>
#include <source_location>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include <unistd.h>

#include <sys/uio.h>

#if defined(__linux__)
    #include <sys/syscall.h>
#endif

#ifndef LOG_BUILD_LEVEL
    #define LOG_BUILD_LEVEL 0
#endif

namespace URing::ALOG
{

enum class Level : std::uint8_t
{
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    Fatal = 4,
    Disabled = 5,
};

inline constexpr Level kBuildMinLevel = static_cast<Level>(LOG_BUILD_LEVEL);

inline std::atomic<Level> g_level{Level::Info};
inline std::atomic<bool> g_colors{true};

namespace detail
{

constexpr std::size_t kMaxThreads = 64;
constexpr std::size_t kQueueSize = 1024;
constexpr std::size_t kMessageBytes = 1024;
constexpr std::size_t kWriteBatch = 16;

static_assert(std::has_single_bit(kQueueSize), "logger queue size must be a power of two");

inline constexpr std::size_t kCacheLine = 64;

struct LevelInfo
{
    std::string_view label;
    std::string_view color;
};

inline constexpr std::array kLevelInfo{
    LevelInfo{"DBG", "\033[36m"  },
    LevelInfo{"INF", "\033[32m"  },
    LevelInfo{"WRN", "\033[33m"  },
    LevelInfo{"ERR", "\033[31m"  },
    LevelInfo{"FTL", "\033[1;31m"},
};

inline constexpr std::string_view kColorReset = "\033[0m";
inline constexpr std::string_view kTruncated = " [truncated]";

[[nodiscard]] constexpr auto level_index(Level level) noexcept -> std::size_t
{
    return static_cast<std::size_t>(level);
}

[[nodiscard]] inline auto basename(const char* path) noexcept -> const char*
{
    const char* slash = std::strrchr(path, '/');
    return slash == nullptr ? path : slash + 1;
}

[[nodiscard]] inline auto thread_id() noexcept -> std::uint64_t
{
#if defined(__linux__)
    thread_local const auto tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
    return tid;
#else
    static std::atomic<std::uint64_t> next_id{1};
    thread_local const std::uint64_t tid = next_id.fetch_add(1, std::memory_order_relaxed);
    return tid;
#endif
}

struct Record
{
    std::uint16_t size = 0;
    std::array<char, kMessageBytes> bytes{};
};

class BoundedSpscQueue
{
public:
    struct Reservation
    {
        Record* record = nullptr;
        std::uint32_t head = 0;
        bool was_empty = false;
    };

    [[nodiscard]] auto try_reserve() noexcept -> Reservation
    {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);

        if (head - tail >= kQueueSize)
        {
            return {};
        }

        return {
            .record = &records_[head & kMask],
            .head = head,
            .was_empty = head == tail,
        };
    }

    void commit(Reservation reservation) noexcept { head_.store(reservation.head + 1, std::memory_order_release); }

    [[nodiscard]] auto try_pop(Record& out) noexcept -> bool
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);

        if (tail == head)
        {
            return false;
        }

        out = records_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] auto empty() const noexcept -> bool
    {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::uint32_t kMask = static_cast<std::uint32_t>(kQueueSize - 1);

    alignas(kCacheLine) std::atomic<std::uint32_t> head_{0};
    alignas(kCacheLine) std::atomic<std::uint32_t> tail_{0};
    std::array<Record, kQueueSize> records_{};
};

struct alignas(kCacheLine) ThreadSlot
{
    BoundedSpscQueue queue;
};

inline std::array<ThreadSlot, kMaxThreads> g_slots{};
inline std::atomic<std::uint64_t> g_free_slots{~std::uint64_t{0}};
inline std::atomic<std::uint32_t> g_slot_count{0};

inline std::atomic<bool> g_running{false};
inline std::atomic<std::uint32_t> g_wake_epoch{0};
inline std::atomic<std::uint64_t> g_dropped{0};
inline std::atomic<int> g_output_fd{STDERR_FILENO};
inline std::jthread g_worker;
inline std::mutex g_lifecycle_mutex;

inline void wake_worker() noexcept
{
    g_wake_epoch.fetch_add(1, std::memory_order_release);
    g_wake_epoch.notify_one();
}

class ThreadRegistration
{
public:
    ThreadRegistration() noexcept
    {
        auto free_slots = g_free_slots.load(std::memory_order_relaxed);
        while (free_slots != 0)
        {
            const auto index = static_cast<std::uint32_t>(std::countr_zero(free_slots));
            const auto claimed = free_slots & ~(std::uint64_t{1} << index);

            if (g_free_slots.compare_exchange_weak(free_slots, claimed, std::memory_order_acquire,
                                                   std::memory_order_relaxed))
            {
                slot_ = index;
                publish_slot_count(index + 1);
                return;
            }
        }
    }

    ThreadRegistration(const ThreadRegistration&) = delete;
    auto operator=(const ThreadRegistration&) -> ThreadRegistration& = delete;

    ~ThreadRegistration()
    {
        if (!valid())
        {
            return;
        }

        auto free_slots = g_free_slots.load(std::memory_order_relaxed);
        const auto bit = std::uint64_t{1} << slot_;
        while (!g_free_slots.compare_exchange_weak(free_slots, free_slots | bit, std::memory_order_release,
                                                   std::memory_order_relaxed))
        {
        }
        wake_worker();
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return slot_ < kMaxThreads; }
    [[nodiscard]] auto slot() const noexcept -> std::uint32_t { return slot_; }

private:
    static void publish_slot_count(std::uint32_t count) noexcept
    {
        auto current = g_slot_count.load(std::memory_order_relaxed);
        while (current < count && !g_slot_count.compare_exchange_weak(current, count, std::memory_order_relaxed,
                                                                      std::memory_order_relaxed))
        {
        }
    }

    std::uint32_t slot_ = UINT32_MAX;
};

[[nodiscard]] inline auto current_slot() noexcept -> std::uint32_t
{
    thread_local ThreadRegistration registration;
    return registration.slot();
}

class FixedBuffer
{
public:
    explicit FixedBuffer(Record& record, std::size_t reserved_tail) noexcept
        : record_(record),
          cursor_(record.bytes.data()),
          end_(record.bytes.data() + record.bytes.size()),
          soft_end_(end_ - reserved_tail)
    {
    }

    void append(std::string_view text) noexcept
    {
        const auto writable = remaining();
        const auto copied = std::min(writable, text.size());
        std::memcpy(cursor_, text.data(), copied);
        cursor_ += copied;
        truncated_ = truncated_ || copied != text.size();
    }

    template <typename... Args>
    void format(std::format_string<Args...> fmt, Args&&... args)
    {
        const auto writable = remaining();
        const auto result = std::format_to_n(cursor_, writable, fmt, std::forward<Args>(args)...);
        cursor_ = result.out;
        truncated_ = truncated_ || result.size > writable;
    }

    void finish(bool colors) noexcept
    {
        if (truncated_)
        {
            append_tail(kTruncated);
        }
        if (colors)
        {
            append_tail(kColorReset);
        }
        append_tail("\n");

        record_.size = static_cast<std::uint16_t>(cursor_ - record_.bytes.data());
    }

private:
    [[nodiscard]] auto remaining() const noexcept -> std::size_t
    {
        return static_cast<std::size_t>(soft_end_ - cursor_);
    }

    void append_tail(std::string_view text) noexcept
    {
        const auto writable = static_cast<std::size_t>(end_ - cursor_);
        const auto copied = std::min(writable, text.size());
        std::memcpy(cursor_, text.data(), copied);
        cursor_ += copied;
    }

    Record& record_;
    char* cursor_;
    char* end_;
    char* soft_end_;
    bool truncated_ = false;
};

struct Timestamp
{
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;
};

[[nodiscard]] inline auto timestamp_now() noexcept -> Timestamp
{
    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = now.time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);

    std::time_t raw_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_r(&raw_time, &local_time);

    return {
        .hour = local_time.tm_hour,
        .minute = local_time.tm_min,
        .second = local_time.tm_sec,
        .millisecond = static_cast<int>((millis - seconds).count()),
    };
}

template <Level L>
void append_prefix(FixedBuffer& out, std::source_location loc, bool colors)
{
    constexpr auto info = kLevelInfo[level_index(L)];
    const auto ts = timestamp_now();

    if (colors)
    {
        out.append(info.color);
    }

    out.format("[{}] [{:02}:{:02}:{:02}.{:03}] [{}] {}:{} | ", info.label, ts.hour, ts.minute, ts.second,
               ts.millisecond, thread_id(), basename(loc.file_name()), loc.line());
}

template <Level L, typename... Args>
void format_record(Record& record, std::source_location loc, std::format_string<Args...> fmt, Args&&... args)
{
    const bool colors = g_colors.load(std::memory_order_relaxed);
    constexpr auto reserved_tail = kTruncated.size() + kColorReset.size() + 1;

    FixedBuffer out{record, reserved_tail};
    append_prefix<L>(out, loc, colors);
    out.format(fmt, std::forward<Args>(args)...);
    out.finish(colors);
}

template <Level L>
void format_failure_record(Record& record, std::source_location loc) noexcept
{
    const bool colors = g_colors.load(std::memory_order_relaxed);
    constexpr auto reserved_tail = kColorReset.size() + 1;
    constexpr auto info = kLevelInfo[level_index(L)];

    FixedBuffer out{record, reserved_tail};
    if (colors)
    {
        out.append(info.color);
    }
    out.append("[");
    out.append(info.label);
    out.append("] logger format failure at ");
    out.append(basename(loc.file_name()));
    out.append(":");

    char line[32]{};
    const int written = std::snprintf(line, sizeof(line), "%u", loc.line());
    if (written > 0)
    {
        out.append(std::string_view{line, static_cast<std::size_t>(written)});
    }

    out.finish(colors);
}

inline void write_all(int fd, std::string_view bytes) noexcept
{
    while (!bytes.empty())
    {
        const auto written = ::write(fd, bytes.data(), bytes.size());
        if (written > 0)
        {
            bytes.remove_prefix(static_cast<std::size_t>(written));
            continue;
        }
        if (written == -1 && errno == EINTR)
        {
            continue;
        }
        return;
    }
}

inline void write_batch(int fd, std::span<Record> batch) noexcept
{
    std::array<iovec, kWriteBatch> iovecs{};
    for (std::size_t i = 0; i < batch.size(); ++i)
    {
        iovecs[i].iov_base = batch[i].bytes.data();
        iovecs[i].iov_len = batch[i].size;
    }

    std::size_t next = 0;
    while (next < batch.size())
    {
        const auto written = ::writev(fd, iovecs.data() + next, static_cast<int>(batch.size() - next));
        if (written == -1 && errno == EINTR)
        {
            continue;
        }
        if (written <= 0)
        {
            g_dropped.fetch_add(batch.size() - next, std::memory_order_relaxed);
            return;
        }

        auto remaining = static_cast<std::size_t>(written);
        while (next < batch.size() && remaining >= iovecs[next].iov_len)
        {
            remaining -= iovecs[next].iov_len;
            ++next;
        }
        if (next < batch.size() && remaining > 0)
        {
            iovecs[next].iov_base = static_cast<char*>(iovecs[next].iov_base) + remaining;
            iovecs[next].iov_len -= remaining;
        }
    }
}

inline void drain_once(int fd) noexcept
{
    std::array<Record, kWriteBatch> batch{};
    const auto slot_count = g_slot_count.load(std::memory_order_acquire);

    for (std::uint32_t slot = 0; slot < slot_count; ++slot)
    {
        std::size_t count = 0;
        while (count < batch.size() && g_slots[slot].queue.try_pop(batch[count]))
        {
            ++count;
        }

        if (count != 0)
        {
            write_batch(fd, std::span{batch.data(), count});
        }
    }
}

[[nodiscard]] inline auto all_queues_empty() noexcept -> bool
{
    const auto slot_count = g_slot_count.load(std::memory_order_acquire);
    for (std::uint32_t slot = 0; slot < slot_count; ++slot)
    {
        if (!g_slots[slot].queue.empty())
        {
            return false;
        }
    }
    return true;
}

inline void logger_main(int fd, std::stop_token stop_token) noexcept
{
    while (!stop_token.stop_requested())
    {
        const auto observed_epoch = g_wake_epoch.load(std::memory_order_acquire);
        drain_once(fd);

        if (!all_queues_empty())
        {
            continue;
        }

        g_wake_epoch.wait(observed_epoch, std::memory_order_relaxed);
    }

    while (!all_queues_empty())
    {
        drain_once(fd);
    }
}

[[nodiscard]] inline auto try_start(int fd) noexcept -> bool;

}  // namespace detail

inline void start(int out_fd = STDERR_FILENO)
{
    std::scoped_lock lock{detail::g_lifecycle_mutex};
    if (detail::g_running.load(std::memory_order_acquire))
    {
        return;
    }

    detail::g_output_fd.store(out_fd, std::memory_order_relaxed);
    detail::g_worker = std::jthread{
        [out_fd](std::stop_token stop_token) noexcept
        {
            detail::logger_main(out_fd, stop_token);
            detail::g_running.store(false, std::memory_order_release);
        },
    };
    detail::g_running.store(true, std::memory_order_release);
}

inline void stop()
{
    std::scoped_lock lock{detail::g_lifecycle_mutex};
    if (!detail::g_running.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    detail::g_worker.request_stop();
    detail::wake_worker();
    if (detail::g_worker.joinable())
    {
        detail::g_worker.join();
    }
}

inline struct AutoStop
{
    ~AutoStop() { stop(); }
} g_auto_stop;

inline auto dropped_count() noexcept -> std::uint64_t
{
    return detail::g_dropped.load(std::memory_order_relaxed);
}

inline void set_level(Level level) noexcept
{
    g_level.store(level, std::memory_order_relaxed);
}

[[nodiscard]] inline auto level() noexcept -> Level
{
    return g_level.load(std::memory_order_relaxed);
}

inline void set_colors(bool enabled) noexcept
{
    g_colors.store(enabled, std::memory_order_relaxed);
}

[[nodiscard]] inline auto colors() noexcept -> bool
{
    return g_colors.load(std::memory_order_relaxed);
}

template <Level L>
[[nodiscard]] inline auto should_log() noexcept -> bool
{
    if constexpr (kBuildMinLevel <= L)
    {
        return g_level.load(std::memory_order_relaxed) <= L;
    }
    else
    {
        return false;
    }
}

namespace detail
{

[[nodiscard]] inline auto try_start(int fd) noexcept -> bool
{
    try
    {
        start(fd);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

}  // namespace detail

template <Level L, typename... Args>
void log_impl(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) noexcept
{
    static_assert(kBuildMinLevel <= L, "log_impl called for a build-disabled level");

    if (!detail::g_running.load(std::memory_order_acquire))
    {
        const auto fd = detail::g_output_fd.load(std::memory_order_relaxed);
        if (!detail::try_start(fd))
        {
            detail::Record fallback{};
            try
            {
                detail::format_record<L>(fallback, loc, fmt, std::forward<Args>(args)...);
            }
            catch (...)
            {
                detail::format_failure_record<L>(fallback, loc);
            }
            detail::write_all(fd, {fallback.bytes.data(), fallback.size});
            return;
        }
    }

    const auto slot = detail::current_slot();
    const auto fd = detail::g_output_fd.load(std::memory_order_relaxed);

    if (slot >= detail::kMaxThreads)
    {
        detail::g_dropped.fetch_add(1, std::memory_order_relaxed);
        if constexpr (L >= Level::Error)
        {
            detail::Record fallback{};
            try
            {
                detail::format_record<L>(fallback, loc, fmt, std::forward<Args>(args)...);
            }
            catch (...)
            {
                detail::format_failure_record<L>(fallback, loc);
            }
            detail::write_all(fd, {fallback.bytes.data(), fallback.size});
        }
        return;
    }

    auto& queue = detail::g_slots[slot].queue;
    auto reservation = queue.try_reserve();
    if (reservation.record == nullptr)
    {
        detail::g_dropped.fetch_add(1, std::memory_order_relaxed);
        if constexpr (L == Level::Fatal)
        {
            detail::Record fallback{};
            try
            {
                detail::format_record<L>(fallback, loc, fmt, std::forward<Args>(args)...);
            }
            catch (...)
            {
                detail::format_failure_record<L>(fallback, loc);
            }
            detail::write_all(fd, {fallback.bytes.data(), fallback.size});
        }
        return;
    }

    try
    {
        detail::format_record<L>(*reservation.record, loc, fmt, std::forward<Args>(args)...);
    }
    catch (...)
    {
        detail::format_failure_record<L>(*reservation.record, loc);
    }

    queue.commit(reservation);

    if (reservation.was_empty)
    {
        detail::wake_worker();
    }
}

}  // namespace URing::ALOG

#define ALOG_DETAIL_WRITE(level, ...)                                                                 \
    do                                                                                                \
    {                                                                                                 \
        constexpr auto alog_detail_level = (level);                                                   \
        if (::URing::ALOG::should_log<alog_detail_level>()) [[unlikely]]                              \
        {                                                                                             \
            ::URing::ALOG::log_impl<alog_detail_level>(std::source_location::current(), __VA_ARGS__); \
        }                                                                                             \
    } while (false)

#if LOG_BUILD_LEVEL <= 0
    #define ALOG_DEBUG(...) ALOG_DETAIL_WRITE(::URing::ALOG::Level::Debug, __VA_ARGS__)
#else
    #define ALOG_DEBUG(...) \
        do                  \
        {                   \
        } while (false)
#endif

#if LOG_BUILD_LEVEL <= 1
    #define ALOG_INFO(...) ALOG_DETAIL_WRITE(::URing::ALOG::Level::Info, __VA_ARGS__)
#else
    #define ALOG_INFO(...) \
        do                 \
        {                  \
        } while (false)
#endif

#if LOG_BUILD_LEVEL <= 2
    #define ALOG_WARN(...) ALOG_DETAIL_WRITE(::URing::ALOG::Level::Warn, __VA_ARGS__)
#else
    #define ALOG_WARN(...) \
        do                 \
        {                  \
        } while (false)
#endif

#if LOG_BUILD_LEVEL <= 3
    #define ALOG_ERROR(...) ALOG_DETAIL_WRITE(::URing::ALOG::Level::Error, __VA_ARGS__)
#else
    #define ALOG_ERROR(...) \
        do                  \
        {                   \
        } while (false)
#endif

#if LOG_BUILD_LEVEL <= 4
    #define ALOG_FATAL(...) ALOG_DETAIL_WRITE(::URing::ALOG::Level::Fatal, __VA_ARGS__)
#else
    #define ALOG_FATAL(...) \
        do                  \
        {                   \
        } while (false)
#endif
