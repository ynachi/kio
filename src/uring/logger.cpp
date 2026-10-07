#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>
#include <uring/logger.hpp>

namespace kio::ALOG
{
    namespace
    {
        using color_sink = spdlog::sinks::stderr_color_sink_mt;

        std::shared_ptr<color_sink>& sink_instance()
        {
            static auto sink = std::make_shared<color_sink>();
            return sink;
        }

        std::shared_ptr<spdlog::logger>& logger_instance()
        {
            static auto logger = []
            {
                auto value = std::make_shared<spdlog::logger>("poro", sink_instance());
                value->set_pattern("%^[%Y-%m-%d %T.%e] [%l] [%s:%#] %v%$");
                value->set_level(spdlog::level::info);
                value->flush_on(spdlog::level::err);
                return value;
            }();
            return logger;
        }

        [[nodiscard]] spdlog::level::level_enum to_spdlog(const level value) noexcept
        {
            switch (value)
            {
            case level::debug:
                return spdlog::level::debug;
            case level::info:
                return spdlog::level::info;
            case level::warn:
                return spdlog::level::warn;
            case level::error:
                return spdlog::level::err;
            case level::fatal:
                return spdlog::level::critical;
            case level::off:
                return spdlog::level::off;
            }
            return spdlog::level::off;
        }

        [[nodiscard]] level from_spdlog(spdlog::level::level_enum value) noexcept
        {
            switch (value)
            {
            case spdlog::level::trace:
            case spdlog::level::debug:
                return level::debug;
            case spdlog::level::info:
                return level::info;
            case spdlog::level::warn:
                return level::warn;
            case spdlog::level::err:
                return level::error;
            case spdlog::level::critical:
                return level::fatal;
            case spdlog::level::off:
            case spdlog::level::n_levels:
                return level::off;
            }
            return level::off;
        }
    } // namespace

    void set_level(level value) noexcept
    {
        try
        {
            detail::g_runtime_level.store(static_cast<std::uint8_t>(value), std::memory_order_relaxed);
            logger_instance()->set_level(to_spdlog(value));
        }
        catch (...)
        {
        }
    }

    level get_level() noexcept
    {
        try
        {
            return from_spdlog(logger_instance()->level());
        }
        catch (...)
        {
            return level::off;
        }
    }

    void set_colors(bool enabled) noexcept
    {
        try
        {
            sink_instance()->set_color_mode(enabled ? spdlog::color_mode::automatic : spdlog::color_mode::never);
        }
        catch (...)
        {
        }
    }

    void flush() noexcept
    {
        try
        {
            logger_instance()->flush();
        }
        catch (...)
        {
        }
    }

    namespace detail
    {
        void write(const level value, const std::source_location location, std::string_view message) noexcept
        {
            try
            {
                const auto source =
                    spdlog::source_loc{
                        location.file_name(), static_cast<int>(location.line()), location.function_name()
                    };
                logger_instance()->log(source, to_spdlog(value), "{}", message);
            }
            catch (...)
            {
            }

            if (value == level::fatal)
            {
                flush();
                std::abort();
            }
        }
    } // namespace detail
} // namespace URing::ALOG
