#pragma once
////////////////////////////////////////////////////////////////////////////////
// logger.hpp - Zero-Cost C++23 Compile-Time Logger
//
// Features:
// 1. Zero Overhead: Disabled logs are stripped from the binary via 'if constexpr'.
// 2. Modern: Uses std::print, std::source_location, and CTAD (No macros).
// 3. Hybrid: Supports both compile-time optimization and runtime filtering.
// 4. Header-only: Single file, no external dependencies (requires C++23).
//
// Usage:
//   Define LOG_BUILD_LEVEL during compilation (-DLOG_BUILD_LEVEL=2) to strip logs.
//   Use Log::info(...), Log::debug(...) in code.
//   Modify Log::g_level at runtime to toggle visibility of remaining logs.
////////////////////////////////////////////////////////////////////////////////

#include <print>
#include <source_location>
#include <format>
#include <chrono>
#include <cstring>
#include <thread>
#include <print>
#include <string_view>

// 1. HARD COMPILE-TIME FLOOR
// Logs below this level are stripped from the binary.
// 0=Debug, 1=Info, 2=Warn, 3=Error, 4=Disabled
#ifndef LOG_BUILD_LEVEL
#define LOG_BUILD_LEVEL 0
#endif

namespace Log {

enum class Level : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3, Disabled = 4 };

// 2. RUNTIME CONFIGURATION
// Defaults to Info, but you can change this anytime.
inline auto g_level = Level::Info;
inline bool g_colors = true;

// Internal compile-time constant
constexpr Level kBuildMinLevel = static_cast<Level>(LOG_BUILD_LEVEL);

namespace detail {
    constexpr const char* RESET = "\033[0m";
    struct LevelConfig { const char* label; const char* color; };
    constexpr LevelConfig levels[] = {
        {"DBG", "\033[36m"}, {"INF", "\033[32m"},
        {"WRN", "\033[33m"}, {"ERR", "\033[31m"}
    };

// Optimized: Uses a single assembly instruction (on x64) to find the slash
inline const char* get_basename(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// --- Thread ID Caching ---
// Returns a pointer to a thread-local static string.
inline const char* get_thread_id() {
    thread_local char buf[16];
    thread_local bool init = false;

    if (!init) {
        long tid = syscall(SYS_gettid); // Linux specific LWP ID
        auto end = std::format_to(buf, "{}", tid);
        *end = '\0';
        init = true;
    }
    return buf;
}

inline void output(Level lvl, std::source_location loc, std::string_view msg) {
    auto& cfg = levels[static_cast<int>(lvl)];

    // 1. Time (Stack buffer, zero allocation)
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::floor<std::chrono::milliseconds>(now);
    char time_buf[64];
    *std::format_to(time_buf, "{:%T}", now_ms) = '\0';

    // 2. Thread ID
    const char* tid = get_thread_id();

    const char* file = get_basename(loc.file_name());
    // 3. Print
    // Format: [LABEL] [TIME] [TID] FILE:LINE | MSG
    if (g_colors) {
        std::println(stderr, "{}[{}] [{}] [{}] {} {}:{} | {}",
            cfg.color, cfg.label,   // Color + Label
            time_buf,               // Time
            tid,                    // Thread ID
            RESET,                  // Reset Color
            file,        // File
            loc.line(),             // Line
            msg);                   // Message
    } else {
        std::println(stderr, "[{}] [{}] [{}] {}:{} | {}",
            cfg.label,
            time_buf,
            tid,
            file,
            loc.line(),
            msg);
    }
}
}  // namespace detail

// THE HYBRID CHECK

template<typename... Args>
struct debug {
    explicit debug(std::format_string<Args...> fmt, Args&&... args,
                   const std::source_location loc = std::source_location::current()) {
        // Binary Stripping (Zero Cost if disabled via macros)
        if constexpr (kBuildMinLevel <= Level::Debug) {
            // Runtime Filter (Dynamic check)
            if (g_level <= Level::Debug) {
                detail::output(Level::Debug, loc, std::format(fmt, std::forward<Args>(args)...));
            }
        }
    }
};

template<typename... Args>
struct info {
    explicit info(std::format_string<Args...> fmt, Args&&... args,
                  const std::source_location loc = std::source_location::current()) {
        if constexpr (kBuildMinLevel <= Level::Info) {
            if (g_level <= Level::Info) {
                detail::output(Level::Info, loc, std::format(fmt, std::forward<Args>(args)...));
            }
        }
    }
};

template<typename... Args>
struct warn {
    explicit warn(std::format_string<Args...> fmt, Args&&... args,
                  const std::source_location loc = std::source_location::current()) {
        if constexpr (kBuildMinLevel <= Level::Warn) {
            if (g_level <= Level::Warn) {
                detail::output(Level::Warn, loc, std::format(fmt, std::forward<Args>(args)...));
            }
        }
    }
};

template<typename... Args>
struct error {
    explicit error(std::format_string<Args...> fmt, Args&&... args,
                   const std::source_location loc = std::source_location::current()) {
        if constexpr (kBuildMinLevel <= Level::Error) {
            if (g_level <= Level::Error) {
                detail::output(Level::Error, loc, std::format(fmt, std::forward<Args>(args)...));
            }
        }
    }
};


template<typename... Args> debug(std::format_string<Args...>, Args&&...) -> debug<Args...>;
template<typename... Args> info(std::format_string<Args...>, Args&&...) -> info<Args...>;
template<typename... Args> warn(std::format_string<Args...>, Args&&...) -> warn<Args...>;
template<typename... Args> error(std::format_string<Args...>, Args&&...) -> error<Args...>;

} // namespace Log