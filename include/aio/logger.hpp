
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <new>
#include <source_location>
#include <thread>

#include <unistd.h>

#include <sys/syscall.h>

#ifndef LOG_BUILD_LEVEL
    #define LOG_BUILD_LEVEL 0
#endif

namespace aio::alog
{

enum class Level : uint8_t
{
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    Fatal = 4,
    Disabled = 5
};

// Global configuration
inline std::atomic g_level{Level::Info};
inline std::atomic g_colors{true};

constexpr Level kBuildMinLevel = static_cast<Level>(LOG_BUILD_LEVEL);

// ---- Parameters ----
constexpr size_t kMaxThreads = 64;
constexpr size_t kQueueSize = 1024;  // Must be power of 2
constexpr size_t kMsgMax = 512;      // Buffer per log line

namespace detail
{

#ifdef __cpp_lib_hardware_interference_size
constexpr size_t kCacheLine = std::hardware_destructive_interference_size;
#else
constexpr size_t kCacheLine = 64;
#endif

struct LevelConfig
{
    const char* label;
    const char* color;
};

constexpr LevelConfig kCfg[] = {
    {"DBG", "\033[36m"  },
    {"INF", "\033[32m"  },
    {"WRN", "\033[33m"  },
    {"ERR", "\033[31m"  },
    {"FTL", "\033[1;31m"}
};
constexpr auto kReset = "\033[0m";

inline const char* basename(const char* path)
{
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Cached thread ID
inline uint32_t get_tid()
{
    thread_local auto tid = static_cast<uint32_t>(::syscall(SYS_gettid));
    return tid;
}

struct Record
{
    uint16_t len;
    uint8_t level;
    char msg[kMsgMax];
};

struct SPSC
{
    static constexpr size_t Mask = kQueueSize - 1;
    static_assert((kQueueSize & (kQueueSize - 1)) == 0, "Queue size not pow2");

    alignas(kCacheLine) std::atomic<uint32_t> head{0};
    alignas(kCacheLine) std::atomic<uint32_t> tail{0};
    Record buf[kQueueSize]{};

    bool try_push(const Record& r) noexcept
    {
        const uint32_t h = head.load(std::memory_order_relaxed);
        const uint32_t t = tail.load(std::memory_order_acquire);

        if ((h - t) >= kQueueSize)
            return false;

        buf[h & Mask] = r;
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(Record& out) noexcept
    {
        const uint32_t t = tail.load(std::memory_order_relaxed);
        const uint32_t h = head.load(std::memory_order_acquire);

        if (t == h)
            return false;

        out = buf[t & Mask];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    bool is_empty() const noexcept
    {
        return head.load(std::memory_order_acquire) == tail.load(std::memory_order_relaxed);
    }
};

struct alignas(kCacheLine) ThreadSlot
{
    std::atomic<bool> active{false};
    SPSC q;
};

// Global State
inline std::array<ThreadSlot, kMaxThreads> g_slots;
inline std::atomic<uint32_t> g_next_slot{0};

// Replaced eventfd with atomic epoch for futex wait
inline std::atomic<uint32_t> g_epoch{0};
inline std::atomic g_running{false};
inline std::jthread g_thread;
inline std::atomic<uint64_t> g_dropped{0};

inline void wake_logger() noexcept
{
    g_epoch.fetch_add(1, std::memory_order_release);
    g_epoch.notify_one();
}

// RAII helper to handle thread destruction
struct ThreadRegGuard
{
    uint32_t slot_idx = UINT32_MAX;

    ThreadRegGuard()
    {
        uint32_t idx = g_next_slot.fetch_add(1, std::memory_order_relaxed);
        if (idx < kMaxThreads)
        {
            slot_idx = idx;
            g_slots[idx].active.store(true, std::memory_order_release);
        }
    }

    ~ThreadRegGuard()
    {
        if (slot_idx < kMaxThreads)
        {
            g_slots[slot_idx].active.store(false, std::memory_order_release);
            wake_logger();
        }
    }
};

inline uint32_t get_thread_slot()
{
    static thread_local ThreadRegGuard guard;
    return guard.slot_idx;
}

inline bool drain_all(int out_fd)
{
    bool any_work = false;
    Record r{};
    for (auto& slot : g_slots)
    {
        if (!slot.active.load(std::memory_order_acquire) && slot.q.is_empty())
        {
            continue;
        }

        // Drain this specific queue
        while (slot.q.try_pop(r))
        {
            (void)::write(out_fd, r.msg, r.len);
            any_work = true;
        }
    }
    return any_work;
}

inline void logger_loop(int out_fd)
{
    while (true)
    {
        const uint32_t captured_epoch = g_epoch.load(std::memory_order_acquire);
        if (!g_running.load(std::memory_order_acquire))
        {
            // One last drain before exit
            drain_all(out_fd);
            return;
        }

        bool did_work = drain_all(out_fd);

        if (!did_work)
        {
            g_epoch.wait(captured_epoch, std::memory_order_relaxed);
        }
    }
}

template <typename... Args>
inline void format_into(Record& r, Level lvl, std::source_location loc, std::format_string<Args...> fmt,
                        Args&&... args) noexcept
{
    r.level = static_cast<uint8_t>(lvl);

    const bool colors = g_colors.load(std::memory_order_relaxed);
    const auto& cfg = kCfg[static_cast<int>(lvl)];
    const uint32_t tid = get_tid();

    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::floor<std::chrono::milliseconds>(now);

    char* p = r.msg;
    constexpr size_t kMax = kMsgMax;
    char* end = r.msg + kMax - 2;

    std::format_to_n_result<char*> res{};

    const auto time_since_epoch = ms.time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch);
    auto millis = time_since_epoch.count() % 1000;

    if (colors)
    {
        res = std::format_to_n(p, end - p, "{}[{}] [{:%T}.{:03}] [{}] {}:{} | ", cfg.color, cfg.label, secs, millis,
                               tid, basename(loc.file_name()), loc.line());
    }
    else
    {
        res = std::format_to_n(p, end - p, "[{}] [{:%T}.{:03}] [{}] {}:{} | ", cfg.label, secs, millis, tid,
                               basename(loc.file_name()), loc.line());
    }
    p = res.out;

    if (p < end)
    {
        res = std::format_to_n(p, end - p, fmt, std::forward<Args>(args)...);
        p = res.out;
    }

    if (colors && p < end)
    {
        res = std::format_to_n(p, end - p, "{}", kReset);
        p = res.out;
    }

    if (p < end)
        *p++ = '\n';
    r.len = static_cast<uint16_t>(p - r.msg);
}

}  // namespace detail

// ---- Lifecycle ----

inline void start(int out_fd = STDERR_FILENO)
{
    // Atomically transition from false -> true.
    // If already true (expected becomes true), we just return.
    if (bool expected = false; !detail::g_running.compare_exchange_strong(expected, true))
        return;

    detail::g_thread = std::jthread([out_fd] { detail::logger_loop(out_fd); });
}

inline void stop()
{
    if (!detail::g_running.exchange(false))
        return;
    detail::wake_logger();
    if (detail::g_thread.joinable())
        detail::g_thread.join();
}

// Automatic cleanup on program exit
inline struct AutoStopper
{
    ~AutoStopper() { stop(); }
} g_auto_stopper;

inline uint64_t dropped_count()
{
    return detail::g_dropped.load(std::memory_order_relaxed);
}

// ---- API ----

template <Level L, typename... Args>
void log_impl(std::source_location loc, std::format_string<Args...> fmt, Args&&... args)
{
    if constexpr (kBuildMinLevel <= L)
    {
        // Lazy initialization: Start on first log if not running
        if (__builtin_expect(!detail::g_running.load(std::memory_order_relaxed), 0))
        {
            start();
        }

        if (g_level.load(std::memory_order_relaxed) > L)
            return;

        uint32_t slot = detail::get_thread_slot();
        if (slot == UINT32_MAX)
        {
            detail::g_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        detail::Record r{};
        detail::format_into(r, L, loc, fmt, std::forward<Args>(args)...);

        auto& q = detail::g_slots[slot].q;
        bool was_empty = q.is_empty();

        if (!q.try_push(r))
        {
            detail::g_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (was_empty)
        {
            detail::wake_logger();
        }
    }
}

}  // namespace aio::alog

// ---- Macros for proper source location capture ----
#define ALOG_DEBUG(...) ::aio::alog::log_impl<::aio::alog::Level::Debug>(std::source_location::current(), __VA_ARGS__)
#define ALOG_INFO(...)  ::aio::alog::log_impl<::aio::alog::Level::Info>(std::source_location::current(), __VA_ARGS__)
#define ALOG_WARN(...)  ::aio::alog::log_impl<::aio::alog::Level::Warn>(std::source_location::current(), __VA_ARGS__)
#define ALOG_ERROR(...) ::aio::alog::log_impl<::aio::alog::Level::Error>(std::source_location::current(), __VA_ARGS__)
#define ALOG_FATAL(...) ::aio::alog::log_impl<::aio::alog::Level::Fatal>(std::source_location::current(), __VA_ARGS__)