#pragma once
////////////////////////////////////////////////////////////////////////////////
// logger.hpp - Zero-Cost C++23 Compile-Time Logger
//
// Features:
// 1. Zero Overhead: Disabled logs are stripped from the binary via 'if constexpr'.
// 2. Zero Allocation: Uses stack buffers and 'write' syscall to avoid heap allocs.
// 3. Modern: Uses std::format_to, std::source_location, and CTAD.
//
// Usage:
//   Define LOG_BUILD_LEVEL during compilation (-DLOG_BUILD_LEVEL=2) to strip logs.
//   Use Log::info("Value: {}", 42);
////////////////////////////////////////////////////////////////////////////////

#include <chrono>
#include <cstring>
#include <format>
#include <source_location>

#include <unistd.h>

#include <sys/syscall.h>

// HARD COMPILE-TIME FLOOR
// Logs below this level are stripped from the binary.
// 0=Debug, 1=Info, 2=Warn, 3=Error, 4=Disabled
#ifndef LOG_BUILD_LEVEL
#define LOG_BUILD_LEVEL 0
#endif

namespace Log {

enum class Level : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3, Disabled = 4 };

// 2. RUNTIME CONFIGURATION
inline auto g_level = Level::Info;
inline bool g_colors = true;

constexpr Level kBuildMinLevel = static_cast<Level>(LOG_BUILD_LEVEL);

namespace detail {

    struct LevelConfig { const char* label; const char* color; const char* reset; };

    constexpr const char* ANSI_RESET = "\033[0m";
    constexpr LevelConfig levels[] = {
        {"DBG", "\033[36m", ANSI_RESET}, // Cyan
        {"INF", "\033[32m", ANSI_RESET}, // Green
        {"WRN", "\033[33m", ANSI_RESET}, // Yellow
        {"ERR", "\033[31m", ANSI_RESET}  // Red
    };

    // Optimized: Uses a single assembly instruction (on x64) to find the slash
    inline const char* get_basename(const char* path) {
        const char* slash = std::strrchr(path, '/');
        return slash ? slash + 1 : path;
    }

    // Thread ID Caching
    inline const char* get_thread_id() {
        thread_local char buf[16];
        thread_local bool init = false;

        if (!init) {
            long tid = syscall(SYS_gettid);
            auto res = std::format_to_n(buf, sizeof(buf) - 1, "{}", tid);
            *res.out = '\0';
            init = true;
        }
        return buf;
    }

    // Core logging logic - Generic Template to avoid std::string allocation
    template <typename... Args>
    void log(Level lvl, std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        // 1. Stack Buffer (2KB should cover 99.9% of logs without allocation)
        // larger messages will simply be truncated safely.
        char buffer[2048];
        char* ptr = buffer;
        const char* end = buffer + sizeof(buffer) - 2; // Leave space for \n and \0

        auto& cfg = levels[static_cast<int>(lvl)];

        // 2. Format Preamble
        // [COLOR] [LABEL] [TIME] [TID] FILE:LINE | [RESET]

        // Time
        const auto now = std::chrono::system_clock::now();
        auto now_ms = std::chrono::floor<std::chrono::milliseconds>(now);

        std::format_to_n_result<char*> res{};

        if (g_colors) {
            res = std::format_to_n(ptr, end - ptr, "{}[{}] [{:%T}] [{}] {}:{} | ",
                cfg.color, cfg.label, now_ms, get_thread_id(),
                get_basename(loc.file_name()), loc.line());
        } else {
            res = std::format_to_n(ptr, end - ptr, "[{}] [{:%T}] [{}] {}:{} | ",
                cfg.label, now_ms, get_thread_id(),
                get_basename(loc.file_name()), loc.line());
        }
        ptr = res.out;

        // 3. Format User Message directly into remaining buffer
        if (ptr < end) {
            res = std::format_to_n(ptr, end - ptr, fmt, std::forward<Args>(args)...);
            ptr = res.out;
        }

        // 4. Color Reset & Newline
        if (g_colors && ptr < end) {
            // We append reset manually to ensure it exists even if truncated
            auto r_res = std::format_to_n(ptr, end - ptr, "{}", cfg.reset);
            ptr = r_res.out;
        }

        if (ptr < end) *ptr++ = '\n';

        // 5. Single System Call Write (Atomic for pipe/file append usually < 4KB)
        ::write(STDERR_FILENO, buffer, ptr - buffer);
    }

}  // namespace detail

// --- Public API ---

/// @brief Logs a debug message.
template<typename... Args>
struct debug {
    explicit debug(std::format_string<Args...> fmt, Args&&... args,
                   const std::source_location loc = std::source_location::current()) {
        if constexpr (kBuildMinLevel <= Level::Debug) {
            if (g_level <= Level::Debug) {
                detail::log(Level::Debug, loc, fmt, std::forward<Args>(args)...);
            }
        }
    }
};

/// @brief Logs an info message.
template<typename... Args>
struct info {
    explicit info(std::format_string<Args...> fmt, Args&&... args,
                  const std::source_location loc = std::source_location::current()) {
        if constexpr (kBuildMinLevel <= Level::Info) {
            if (g_level <= Level::Info) {
                detail::log(Level::Info, loc, fmt, std::forward<Args>(args)...);
            }
        }
    }
};

/// @brief Logs a warning message.
template<typename... Args>
struct warn {
    explicit warn(std::format_string<Args...> fmt, Args&&... args,
                  const std::source_location loc = std::source_location::current()) {
        if constexpr (kBuildMinLevel <= Level::Warn) {
            if (g_level <= Level::Warn) {
                detail::log(Level::Warn, loc, fmt, std::forward<Args>(args)...);
            }
        }
    }
};

/// @brief Logs an error message.
template<typename... Args>
struct error {
    explicit error(std::format_string<Args...> fmt, Args&&... args,
                   const std::source_location loc = std::source_location::current()) {
        if constexpr (kBuildMinLevel <= Level::Error) {
            if (g_level <= Level::Error) {
                detail::log(Level::Error, loc, fmt, std::forward<Args>(args)...);
            }
        }
    }
};

// CTAD Guides (Allows Usage: Log::info("...", ...))
template<typename... Args> debug(std::format_string<Args...>, Args&&...) -> debug<Args...>;
template<typename... Args> info(std::format_string<Args...>, Args&&...) -> info<Args...>;
template<typename... Args> warn(std::format_string<Args...>, Args&&...) -> warn<Args...>;
template<typename... Args> error(std::format_string<Args...>, Args&&...) -> error<Args...>;

} // namespace Log