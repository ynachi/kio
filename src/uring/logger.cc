#include "uring/logger.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

#include <absl/base/log_severity.h>
#include <absl/log/globals.h>
#include <absl/log/initialize.h>
#include <absl/log/log.h>
#include <absl/log/log_entry.h>
#include <absl/log/log_sink.h>
#include <absl/log/log_sink_registry.h>

// Backend for uring/logger.hpp, written against absl::log (kio already
// depends on Abseil) instead of poro's spdlog-backed logger.cpp.
namespace URing::log
{
namespace
{
std::atomic<level> g_runtime_level{level::info};
std::atomic<bool> g_colors{true};

// Abseil's LogEntry only carries one of its 4 canonical severities, which
// collapses our `debug` and `info` levels together. Send() runs
// synchronously on the logging thread during the LOG() statement, so a
// thread_local sidecar recovers the exact level for coloring.
thread_local level g_current_level = level::info;

constexpr absl::LogSeverity to_absl_severity(level value) noexcept
{
    switch (value)
    {
        case level::debug:
        case level::info:
            return absl::LogSeverity::kInfo;
        case level::warn:
            return absl::LogSeverity::kWarning;
        case level::error:
            return absl::LogSeverity::kError;
        case level::fatal:
        case level::off:
            return absl::LogSeverity::kFatal;
    }
    return absl::LogSeverity::kError;
}

constexpr std::string_view color_for(level value) noexcept
{
    switch (value)
    {
        case level::debug:
            return "\033[36m";
        case level::info:
            return "\033[32m";
        case level::warn:
            return "\033[33m";
        case level::error:
            return "\033[31m";
        case level::fatal:
            return "\033[1;31m";
        default:
            return "";
    }
}

class ColorSink final : public absl::LogSink
{
   public:
    void Send(const absl::LogEntry& entry) override
    {
        const auto line = entry.text_message_with_prefix_and_newline();
        if (g_colors.load(std::memory_order_relaxed))
        {
            std::fwrite(color_for(g_current_level).data(), 1, color_for(g_current_level).size(), stderr);
            std::fwrite(line.data(), 1, line.size(), stderr);
            static constexpr std::string_view kReset = "\033[0m";
            std::fwrite(kReset.data(), 1, kReset.size(), stderr);
        }
        else
        {
            std::fwrite(line.data(), 1, line.size(), stderr);
        }
    }
};

ColorSink& sink_instance()
{
    static ColorSink sink;
    return sink;
}

void ensure_initialized()
{
    static std::once_flag flag;
    std::call_once(flag,
                    []
                    {
                        // We print via our own sink below; suppress Abseil's
                        // separate default stderr writer to avoid duplicate lines.
                        absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfinity);
                        absl::InitializeLog();
                        absl::AddLogSink(&sink_instance());
                    });
}

}  // namespace

void set_level(level value) noexcept
{
    g_runtime_level.store(value, std::memory_order_relaxed);
}

level get_level() noexcept
{
    return g_runtime_level.load(std::memory_order_relaxed);
}

void set_colors(bool enabled) noexcept
{
    g_colors.store(enabled, std::memory_order_relaxed);
}

void flush() noexcept
{
    absl::FlushLogSinks();
    std::fflush(stderr);
}

namespace detail
{

void write(level value, std::source_location location, std::string_view message) noexcept
{
    ensure_initialized();
    if (value < g_runtime_level.load(std::memory_order_relaxed))
    {
        return;
    }
    g_current_level = value;
    LOG(LEVEL(to_absl_severity(value))).AtLocation(location.file_name(), static_cast<int>(location.line()))
        << message;
    // LOG(LEVEL(kFatal)) already terminates the process; poro's spdlog-backed
    // write() aborts manually afterward because spdlog doesn't, but here that
    // would be unreachable.
}

}  // namespace detail
}  // namespace URing::log
