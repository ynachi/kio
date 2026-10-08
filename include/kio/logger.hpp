#pragma once

#include <atomic>
#include <cstdint>
#include <format>
#include <source_location>
#include <string_view>
#include <utility>

namespace kio::ALOG
{
    enum class level : std::uint8_t
    {
        debug = 0,
        info = 1,
        warn = 2,
        error = 3,
        fatal = 4,
        off = 5,
    };

#ifndef KIO_LOG_ACTIVE_LEVEL
#    define KIO_LOG_ACTIVE_LEVEL 0
#endif

    inline constexpr auto active_level = static_cast<level>(KIO_LOG_ACTIVE_LEVEL);

    [[nodiscard]] constexpr bool is_compiled(const level value) noexcept { return value >= active_level; }

    namespace detail
    {
        // Mirror of the logger's runtime level, so call sites can skip formatting a
        // message that would be filtered anyway. Written only by set_level().
        inline std::atomic<std::uint8_t> g_runtime_level{static_cast<std::uint8_t>(level::info)};
    } // namespace detail

    /// True if a message at `value` would currently be emitted. Fatal always is:
    /// it aborts regardless of the configured level.
    [[nodiscard]] inline bool enabled(const level value) noexcept
    {
        return value == level::fatal ||
               static_cast<std::uint8_t>(value) >= detail::g_runtime_level.load(std::memory_order_relaxed);
    }

    void set_level(level value) noexcept;
    [[nodiscard]] level get_level() noexcept;
    void set_colors(bool enabled) noexcept;
    void flush() noexcept;

    namespace detail
    {
        void write(level value, std::source_location location, std::string_view message) noexcept;

        template <level Level, typename... Args>
        void write_formatted(const std::source_location location, std::format_string<Args...> format,
                             Args&&... args) noexcept
        {
            // Filter before formatting: std::format allocates, and a disabled
            // message must cost one relaxed load, not a format plus a drop.
            if (!enabled(Level))
            {
                return;
            }
            try
            {
                write(Level, location, std::format(format, std::forward<Args>(args)...));
            }
            catch (...)
            {
                write(level::error, location, "log message formatting failed");
            }
        }
    } // namespace detail
} // namespace URing::ALOG

#define KIO_DETAIL_LOG(LEVEL, ...)                                                                          \
    do {                                                                                                     \
        if constexpr (::kio::ALOG::is_compiled(::kio::ALOG::level::LEVEL)) {                                 \
            ::kio::ALOG::detail::write_formatted<::kio::ALOG::level::LEVEL>(std::source_location::current(), \
                                                                            __VA_ARGS__);                    \
        }                                                                                                    \
    } while (false)

#define KIO_LOG_DEBUG(...) KIO_DETAIL_LOG(debug, __VA_ARGS__)
#define KIO_LOG_INFO(...) KIO_DETAIL_LOG(info, __VA_ARGS__)
#define KIO_LOG_WARN(...) KIO_DETAIL_LOG(warn, __VA_ARGS__)
#define KIO_LOG_ERROR(...) KIO_DETAIL_LOG(error, __VA_ARGS__)
#define KIO_LOG_FATAL(...) KIO_DETAIL_LOG(fatal, __VA_ARGS__)
