#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <print>
#include <source_location>
#include <string_view>
#include <thread>

#ifndef LOG_BUILD_LEVEL
    #define LOG_BUILD_LEVEL 0
#endif

namespace URing::ALOG
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

inline constexpr Level kBuildLevel = static_cast<Level>(LOG_BUILD_LEVEL);
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
    uint16_t len{};
    std::array<char, 1024> msg{};
};

constexpr size_t kQueueSize = 1024;
struct Slot
{
    std::atomic<uint8_t> state{0};  // 0: Free, 1: Writing, 2: Ready
    Record record;
};

inline std::array<Slot, kQueueSize> g_queue;
inline std::atomic<uint64_t> g_head{0};
inline std::atomic<uint64_t> g_tail{0};
inline std::atomic<uint64_t> g_wake{0};
inline std::jthread g_worker;

[[nodiscard]] constexpr const char* basename(const char* path) noexcept
{
    if (!path)
        return "";
    const char* last = path;
    for (const char* p = path; *p; ++p)
    {
        if (*p == '/')
        {
            last = p + 1;
        }
    }
    return last;
}

inline void worker_loop(std::stop_token st)
{
    uint64_t wake_val = g_wake.load(std::memory_order_acquire);

    while (!st.stop_requested())
    {
        bool processed = false;

        while (true)
        {
            uint64_t h = g_head.load(std::memory_order_relaxed);
            auto& slot = g_queue[h % kQueueSize];
            if (slot.state.load(std::memory_order_acquire) != 2)
                break;

            const auto& r = slot.record;
            std::string_view color = "", level_str = "UNKN", reset = "";

            if (g_colors.load(std::memory_order_relaxed))
            {
                reset = "\033[0m";
                switch (r.level)
                {
                    case Level::Debug:
                        color = "\033[36m";
                        level_str = "DBG";
                        break;
                    case Level::Info:
                        color = "\033[32m";
                        level_str = "INF";
                        break;
                    case Level::Warn:
                        color = "\033[33m";
                        level_str = "WRN";
                        break;
                    case Level::Error:
                        color = "\033[31m";
                        level_str = "ERR";
                        break;
                    case Level::Fatal:
                        color = "\033[1;31m";
                        level_str = "FTL";
                        break;
                    default:
                        break;
                }
            }
            else
            {
                switch (r.level)
                {
                    case Level::Debug:
                        level_str = "DBG";
                        break;
                    case Level::Info:
                        level_str = "INF";
                        break;
                    case Level::Warn:
                        level_str = "WRN";
                        break;
                    case Level::Error:
                        level_str = "ERR";
                        break;
                    case Level::Fatal:
                        level_str = "FTL";
                        break;
                    default:
                        break;
                }
            }

            std::println(stderr, "{}[{}] [{:%H:%M:%S}] [{}] {}:{} | {}{}", color, level_str, r.ts, r.tid,
                         basename(r.loc.file_name()), r.loc.line(), std::string_view(r.msg.data(), r.len), reset);

            slot.state.store(0, std::memory_order_release);
            g_head.store(h + 1, std::memory_order_relaxed);
            processed = true;
        }

        if (!processed)
        {
            g_wake.wait(wake_val, std::memory_order_acquire);
        }
        wake_val = g_wake.load(std::memory_order_acquire);
    }

    // Drain remaining messages before exiting
    while (true)
    {
        uint64_t h = g_head.load(std::memory_order_relaxed);
        auto& slot = g_queue[h % kQueueSize];
        if (slot.state.load(std::memory_order_acquire) != 2)
            break;

        const auto& r = slot.record;
        std::string_view level_str = "UNKN";
        switch (r.level)
        {
            case Level::Debug:
                level_str = "DBG";
                break;
            case Level::Info:
                level_str = "INF";
                break;
            case Level::Warn:
                level_str = "WRN";
                break;
            case Level::Error:
                level_str = "ERR";
                break;
            case Level::Fatal:
                level_str = "FTL";
                break;
            default:
                break;
        }

        std::println(stderr, "[{}] [{:%H:%M:%S}] [{}] {}:{} | {}", level_str, r.ts, r.tid, basename(r.loc.file_name()),
                     r.loc.line(), std::string_view(r.msg.data(), r.len));

        slot.state.store(0, std::memory_order_release);
        g_head.store(h + 1, std::memory_order_relaxed);
    }
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
    uint64_t t = detail::g_tail.fetch_add(1, std::memory_order_relaxed);
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
        r.len = static_cast<uint16_t>(std::min(static_cast<size_t>(res.size), r.msg.size()));
    }
    catch (...)
    {
        r.len = 0;
    }

    slot.state.store(2, std::memory_order_release);
    detail::g_wake.fetch_add(1, std::memory_order_release);
    detail::g_wake.notify_one();
}

}  // namespace URing::ALOG

#define ALOG_DETAIL_WRITE(LEVEL, ...)                                                                               \
    do                                                                                                              \
    {                                                                                                               \
        if constexpr (::URing::ALOG::Level::LEVEL >= ::URing::ALOG::kBuildLevel)                                    \
        {                                                                                                           \
            if (::URing::ALOG::should_log(::URing::ALOG::Level::LEVEL)) [[unlikely]]                                \
            {                                                                                                       \
                ::URing::ALOG::log_impl<::URing::ALOG::Level::LEVEL>(std::source_location::current(), __VA_ARGS__); \
            }                                                                                                       \
        }                                                                                                           \
    } while (0)

#define ALOG_DEBUG(...) ALOG_DETAIL_WRITE(Debug, __VA_ARGS__)
#define ALOG_INFO(...)  ALOG_DETAIL_WRITE(Info, __VA_ARGS__)
#define ALOG_WARN(...)  ALOG_DETAIL_WRITE(Warn, __VA_ARGS__)
#define ALOG_ERROR(...) ALOG_DETAIL_WRITE(Error, __VA_ARGS__)
#define ALOG_FATAL(...) ALOG_DETAIL_WRITE(Fatal, __VA_ARGS__)
