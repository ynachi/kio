#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <print>
#include <source_location>
#include <string_view>
#include <thread>
#include <utility>

#ifndef ZIO_LOG_BUILD_LEVEL
    #define ZIO_LOG_BUILD_LEVEL 0
#endif

namespace zio::ALOG
{

enum class Level : std::uint8_t
{
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    Fatal = 4,
    Disabled = 5
};

inline constexpr Level kBuildLevel = static_cast<Level>(ZIO_LOG_BUILD_LEVEL);
inline std::atomic<Level> g_runtime_level{Level::Info};
inline std::atomic<bool> g_colors{true};

inline bool should_log(Level l) noexcept
{
    return l >= g_runtime_level.load(std::memory_order_relaxed);
}

namespace detail
{
struct Record
{
    Level level;
    std::source_location loc;
    std::chrono::system_clock::time_point ts;
    std::thread::id tid;
    std::uint16_t len{};
    std::array<char, 1024> msg{};
};

constexpr std::size_t kQueueSize = 1024;

struct Slot
{
    std::atomic<std::uint8_t> state{0};  // 0: Free, 1: Writing, 2: Ready
    Record record;
};

inline std::array<Slot, kQueueSize> g_queue;
inline std::atomic<std::uint64_t> g_head{0};
inline std::atomic<std::uint64_t> g_tail{0};
inline std::atomic<std::uint64_t> g_wake{0};
inline std::jthread g_worker;

[[nodiscard]] constexpr const char* basename(const char* path) noexcept
{
    if (!path)
        return "";
    const char* last = path;
    for (const char* p = path; *p; ++p)
    {
        if (*p == '/')
            last = p + 1;
    }
    return last;
}

inline std::string_view level_name(Level level) noexcept
{
    switch (level)
    {
        case Level::Debug:
            return "DBG";
        case Level::Info:
            return "INF";
        case Level::Warn:
            return "WRN";
        case Level::Error:
            return "ERR";
        case Level::Fatal:
            return "FTL";
        default:
            return "UNKN";
    }
}

inline std::string_view level_color(Level level) noexcept
{
    switch (level)
    {
        case Level::Debug:
            return "\033[36m";
        case Level::Info:
            return "\033[32m";
        case Level::Warn:
            return "\033[33m";
        case Level::Error:
            return "\033[31m";
        case Level::Fatal:
            return "\033[1;31m";
        default:
            return "";
    }
}

inline void print_record(const Record& r, bool colors)
{
    const auto level = level_name(r.level);
    if (colors)
    {
        std::println(stderr, "{}[{}] [{:%H:%M:%S}] [{}] {}:{} | {}\033[0m",
                     level_color(r.level), level, r.ts, r.tid, basename(r.loc.file_name()),
                     r.loc.line(), std::string_view(r.msg.data(), r.len));
    }
    else
    {
        std::println(stderr, "[{}] [{:%H:%M:%S}] [{}] {}:{} | {}", level, r.ts, r.tid,
                     basename(r.loc.file_name()), r.loc.line(),
                     std::string_view(r.msg.data(), r.len));
    }
}

inline void drain(bool colors)
{
    while (true)
    {
        std::uint64_t h = g_head.load(std::memory_order_relaxed);
        auto& slot = g_queue[h % kQueueSize];
        if (slot.state.load(std::memory_order_acquire) != 2)
            break;

        print_record(slot.record, colors);
        slot.state.store(0, std::memory_order_release);
        g_head.store(h + 1, std::memory_order_relaxed);
    }
}

inline void worker_loop(std::stop_token st)
{
    std::uint64_t wake_val = g_wake.load(std::memory_order_acquire);

    while (!st.stop_requested())
    {
        const auto old_head = g_head.load(std::memory_order_relaxed);
        drain(g_colors.load(std::memory_order_relaxed));
        if (g_head.load(std::memory_order_relaxed) == old_head)
            g_wake.wait(wake_val, std::memory_order_acquire);
        wake_val = g_wake.load(std::memory_order_acquire);
    }

    drain(false);
}

inline void start()
{
    static std::once_flag flag;
    std::call_once(flag, [] { g_worker = std::jthread(worker_loop); });
}
}  // namespace detail

inline void set_level(Level level) noexcept
{
    g_runtime_level.store(level, std::memory_order_relaxed);
}

inline void set_colors(bool enable) noexcept
{
    g_colors.store(enable, std::memory_order_relaxed);
}

inline void start()
{
    detail::start();
}

inline void stop()
{
    detail::g_worker.request_stop();
    detail::g_wake.fetch_add(1, std::memory_order_release);
    detail::g_wake.notify_all();
}

inline struct AutoStop
{
    ~AutoStop() { stop(); }
} g_auto_stop;

template <Level L, typename... Args>
void log_impl(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) noexcept
{
    detail::start();
    std::uint64_t t = detail::g_tail.fetch_add(1, std::memory_order_relaxed);
    auto& slot = detail::g_queue[t % detail::kQueueSize];

    while (slot.state.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    slot.state.store(1, std::memory_order_relaxed);

    auto& r = slot.record;
    r.level = L;
    r.loc = loc;
    r.ts = std::chrono::system_clock::now();
    r.tid = std::this_thread::get_id();

    try
    {
        auto res = std::format_to_n(r.msg.data(), r.msg.size(), fmt, std::forward<Args>(args)...);
        r.len = static_cast<std::uint16_t>(std::min(static_cast<std::size_t>(res.size), r.msg.size()));
    }
    catch (...)
    {
        r.len = 0;
    }

    slot.state.store(2, std::memory_order_release);
    detail::g_wake.fetch_add(1, std::memory_order_release);
    detail::g_wake.notify_one();
}

}  // namespace zio::ALOG

#define ZIO_ALOG_DETAIL_WRITE(LEVEL, ...)                                                          \
    do                                                                                             \
    {                                                                                              \
        if constexpr (::zio::ALOG::Level::LEVEL >= ::zio::ALOG::kBuildLevel)                       \
        {                                                                                          \
            if (::zio::ALOG::should_log(::zio::ALOG::Level::LEVEL)) [[unlikely]]                   \
            {                                                                                      \
                ::zio::ALOG::log_impl<::zio::ALOG::Level::LEVEL>(std::source_location::current(),  \
                                                                 __VA_ARGS__);                     \
            }                                                                                      \
        }                                                                                          \
    } while (0)

#define ZIO_LOG_DEBUG(...) ZIO_ALOG_DETAIL_WRITE(Debug, __VA_ARGS__)
#define ZIO_LOG_INFO(...)  ZIO_ALOG_DETAIL_WRITE(Info, __VA_ARGS__)
#define ZIO_LOG_WARN(...)  ZIO_ALOG_DETAIL_WRITE(Warn, __VA_ARGS__)
#define ZIO_LOG_ERROR(...) ZIO_ALOG_DETAIL_WRITE(Error, __VA_ARGS__)
#define ZIO_LOG_FATAL(...) ZIO_ALOG_DETAIL_WRITE(Fatal, __VA_ARGS__)
