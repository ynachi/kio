// Created by Yao ACHI on 25/10/2025.

#ifndef KIO_ASYNC_LOGGER_H
#define KIO_ASYNC_LOGGER_H

#include "kio/sync/mpsc_queue.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <format>
#include <iostream>
#include <memory>
#include <source_location>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>
#include <utility>

#include <unistd.h>

#include <sys/eventfd.h>

namespace kio
{

constexpr auto kLoggerSleepInterval = std::chrono::milliseconds(50);

inline int64_t GetThreadId() noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    thread_local const int64_t kTid = syscall(SYS_gettid);
    return kTid;
}

enum class LogLevel : uint8_t
{
    kDisabled = 0,
    kTrace,
    kDebug,
    kInfo,
    kWarn,
    kError,
};

struct LogMessage
{
    static constexpr size_t kMsgCapacity = 256;
    std::chrono::system_clock::time_point timestamp;  // NOLINT(misc-non-private-member-variables-in-classes)
    LogLevel level{LogLevel::kInfo};                  // NOLINT(misc-non-private-member-variables-in-classes)
    std::string_view file;                            // NOLINT(misc-non-private-member-variables-in-classes)
    std::string_view function;                        // NOLINT(misc-non-private-member-variables-in-classes)
    uint64_t line{};                                  // NOLINT(misc-non-private-member-variables-in-classes)
    size_t thread_id{};                               // NOLINT(misc-non-private-member-variables-in-classes)

    std::array<char, kMsgCapacity> buffer{};  // NOLINT(misc-non-private-member-variables-in-classes)
    std::unique_ptr<std::string> heap_msg;    // NOLINT(misc-non-private-member-variables-in-classes)
    std::span<const char> msg;                // NOLINT(misc-non-private-member-variables-in-classes)

    void SetSmallMessage(std::span<const char> data)
    {
        std::memcpy(buffer.data(), data.data(), data.size());
        msg = std::span<const char>(buffer.data(), data.size());
        heap_msg.reset();
    }

    void SetLargeMessage(std::string&& s)
    {
        heap_msg = std::make_unique<std::string>(std::move(s));
        msg = std::span<const char>(heap_msg->data(), heap_msg->size());
    }

    [[nodiscard]]
    std::string_view View() const noexcept
    {
        return {msg.data(), msg.size()};
    }

    LogMessage(const LogMessage&) = delete;
    LogMessage& operator=(const LogMessage&) = delete;
    LogMessage() = default;
    ~LogMessage() = default;

    LogMessage(LogMessage&& other) noexcept
        : timestamp(other.timestamp),
          level(other.level),
          file(other.file),
          function(other.function),
          line(other.line),
          thread_id(other.thread_id)
    {
        if (other.heap_msg)
        {
            heap_msg = std::move(other.heap_msg);
            msg = std::span<const char>(heap_msg->data(), heap_msg->size());
        }
        else
        {
            buffer = other.buffer;
            msg = std::span<const char>(buffer.data(), other.msg.size());
        }
        other.msg = std::span<const char>();
    }

    LogMessage& operator=(LogMessage&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        timestamp = other.timestamp;
        level = other.level;
        file = other.file;
        function = other.function;
        line = other.line;
        thread_id = other.thread_id;

        if (other.heap_msg)
        {
            heap_msg = std::move(other.heap_msg);
            msg = std::span<const char>(heap_msg->data(), heap_msg->size());
        }
        else
        {
            buffer = other.buffer;
            msg = std::span<const char>(buffer.data(), other.msg.size());
            heap_msg.reset();
        }

        other.msg = std::span<const char>();
        return *this;
    }
};

class Logger
{
public:
    explicit Logger(const size_t queue_size = 1024, const LogLevel level = LogLevel::kInfo,
                    std::ostream& output_stream = std::cout)
        : queue_(queue_size), level_(level), wakeup_fd_(eventfd(0, EFD_CLOEXEC)), output_stream_(output_stream)

    {
        if (wakeup_fd_ < 0)
        {
            std::osyncstream(std::cerr) << "Failed to create logger eventfd: " << strerror(errno) << '\n';
        }

        if (&output_stream_ == &std::cout)
        {
            use_color_ = ::isatty(STDOUT_FILENO) != 0;
        }

        consumer_thread_ = std::jthread([this](const std::stop_token& st) { ConsumeLoop(st); });
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    ~Logger()
    {
        consumer_thread_.request_stop();

        if (wakeup_fd_ >= 0)
        {
            constexpr uint64_t val = 1;
            [[maybe_unused]] const auto ret = ::write(wakeup_fd_, &val, sizeof(val));
        }

        if (consumer_thread_.joinable())
        {
            consumer_thread_.join();
        }

        if (wakeup_fd_ >= 0)
        {
            ::close(wakeup_fd_);
            wakeup_fd_ = -1;
        }
    }

    void SetLevel(const LogLevel level) { level_.store(level, std::memory_order_relaxed); }

    [[nodiscard]]
    bool ShouldLog(LogLevel lvl) const noexcept
    {
        if (const auto kCurrentLevel = level_.load(std::memory_order_relaxed); kCurrentLevel == LogLevel::kDisabled)
        {
            return false;
        }
        return static_cast<int>(lvl) >= static_cast<int>(level_.load(std::memory_order_relaxed));
    }

    template <typename... Args>
    void Trace(const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args)
    {
        Log<LogLevel::kTrace>(loc, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Debug(const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args)
    {
        Log<LogLevel::kDebug>(loc, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Info(const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args)
    {
        Log<LogLevel::kInfo>(loc, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Warn(const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args)
    {
        Log<LogLevel::kWarn>(loc, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Error(const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args)
    {
        Log<LogLevel::kError>(loc, fmt, std::forward<Args>(args)...);
    }

    void Flush()
    {
        LogMessage entry;
        while (queue_.TryPop(entry))
        {
            Write(entry);
        }
    }

private:
    template <LogLevel L, typename... Args>
    void Log(const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args)
    {
        if constexpr (L == LogLevel::kDisabled)
        {
            return;
        }
        if (!ShouldLog(L))
        {
            return;
        }

        std::array<char, LogMessage::kMsgCapacity> stack_buf{};

        try
        {
            auto result = std::format_to_n(stack_buf.data(), stack_buf.size(), fmt, std::forward<Args>(args)...);
            const auto written = static_cast<size_t>(result.out - stack_buf.data());

            LogMessage entry{};
            entry.timestamp = std::chrono::system_clock::now();
            entry.level = L;
            entry.file = loc.file_name();
            entry.function = loc.function_name();
            entry.line = loc.line();
            entry.thread_id = GetThreadId();

            if (result.size >= 0 && static_cast<size_t>(result.size) <= LogMessage::kMsgCapacity)
            {
                entry.SetSmallMessage(std::span<const char>(stack_buf.data(), written));
            }
            else
            {
                std::string large = std::format(fmt, std::forward<Args>(args)...);
                entry.SetLargeMessage(std::move(large));
            }

            if (queue_.TryPush(std::move(entry)))
            {
                if (wakeup_fd_ >= 0)
                {
                    constexpr uint64_t val = 1;
                    if (const ssize_t ret = ::write(wakeup_fd_, &val, sizeof(val)); ret < 0 && errno != EAGAIN)
                    {
                        std::osyncstream(std::cerr) << "Logger wakeup write() error: " << strerror(errno) << '\n';
                    }
                }
            }
            else
            {
                std::osyncstream(std::cerr) << "Logger queue full. Dropping message.\n";
            }
        }
        catch (const std::exception& e)
        {
            std::string fallback = std::string("<log format error: ") + e.what() + ">";
            LogMessage entry{};
            entry.timestamp = std::chrono::system_clock::now();
            entry.level = L;
            entry.file = loc.file_name();
            entry.function = loc.function_name();
            entry.line = loc.line();
            entry.thread_id = GetThreadId();
            if (fallback.size() <= LogMessage::kMsgCapacity)
            {
                entry.SetSmallMessage(std::span<const char>(fallback.data(), fallback.size()));
            }
            else
            {
                entry.SetLargeMessage(std::move(fallback));
            }
            (void)queue_.TryPush(std::move(entry));
        }
    }

    void ConsumeLoop(const std::stop_token& st)
    {
        uint64_t val = 0;
        while (!st.stop_requested())
        {
            if (wakeup_fd_ < 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(kLoggerSleepInterval));
            }
            else
            {
                if (const ssize_t kRet = ::read(wakeup_fd_, &val, sizeof(val)); kRet < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    std::osyncstream(std::cerr) << "Logger wakeup read() error: " << strerror(errno) << '\n';
                    break;
                }
            }
            Flush();
        }
        Flush();
    }

    void Write(const LogMessage& msg) const
    {
        const char* color = LevelColor(msg.level);
        const char* reset = use_color_ ? "\033[0m" : "";

        std::string time_str;
        try
        {
            const auto kMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(msg.timestamp.time_since_epoch()) % 1000;
            time_str = std::format("{:%Y-%m-%d %H:%M:%S}.{:03d}", msg.timestamp, kMs.count());
        }
        catch (...)
        {
            time_str = R"(????-??-?? ??:??:??.???)";
        }

        std::string_view filename = msg.file;
        if (const auto kPos = filename.find_last_of("/\\"); kPos != std::string_view::npos)
        {
            filename = filename.substr(kPos + 1);
        }

        output_stream_ << std::format("{}[{}] [{:<5}] [tid:{}] {}:{} - {}{}\n", color, time_str, LevelName(msg.level),
                                      msg.thread_id, filename, msg.line, msg.View(), reset);
    }

    static const char* LevelName(const LogLevel lvl)
    {
        switch (lvl)
        {
            case LogLevel::kTrace:
                return "TRACE";
            case LogLevel::kDebug:
                return "DEBUG";
            case LogLevel::kInfo:
                return "INFO";
            case LogLevel::kWarn:
                return "WARN";
            case LogLevel::kError:
                return "ERROR";
            default:
                return "DISA";
        }
    }

    [[nodiscard]] const char* LevelColor(const LogLevel lvl) const noexcept
    {
        if (!use_color_)
        {
            return "";
        }
        switch (lvl)
        {
            case LogLevel::kTrace:
                return "\033[37m";
            case LogLevel::kDebug:
                return "\033[36m";
            case LogLevel::kInfo:
                return "\033[32m";
            case LogLevel::kWarn:
                return "\033[33m";
            case LogLevel::kError:
                return "\033[31m";
            default:
                return "\033[0m";
        }
    }

    MPSCQueue<LogMessage> queue_;
    std::atomic<LogLevel> level_;
    std::jthread consumer_thread_;
    int wakeup_fd_{-1};
    std::ostream& output_stream_;
    bool use_color_{false};
};

/* ──────────────── Core macros (kept as-is) ──────────────── */
#define KIO_LOG_TRACE(logger_instance, ...)                                        \
    do                                                                             \
    {                                                                              \
        if ((logger_instance).ShouldLog(::kio::LogLevel::kTrace))                  \
            (logger_instance).Trace(std::source_location::current(), __VA_ARGS__); \
    } while (0)

#define KIO_LOG_DEBUG(logger_instance, ...)                                        \
    do                                                                             \
    {                                                                              \
        if ((logger_instance).ShouldLog(::kio::LogLevel::kDebug))                  \
            (logger_instance).Debug(std::source_location::current(), __VA_ARGS__); \
    } while (0)

#define KIO_LOG_INFO(logger_instance, ...)                                        \
    do                                                                            \
    {                                                                             \
        if ((logger_instance).ShouldLog(::kio::LogLevel::kInfo))                  \
            (logger_instance).Info(std::source_location::current(), __VA_ARGS__); \
    } while (0)

#define KIO_LOG_WARN(logger_instance, ...)                                        \
    do                                                                            \
    {                                                                             \
        if ((logger_instance).ShouldLog(::kio::LogLevel::kWarn))                  \
            (logger_instance).Warn(std::source_location::current(), __VA_ARGS__); \
    } while (0)

#define KIO_LOG_ERROR(logger_instance, ...)                                        \
    do                                                                             \
    {                                                                              \
        if ((logger_instance).ShouldLog(::kio::LogLevel::kError))                  \
            (logger_instance).Error(std::source_location::current(), __VA_ARGS__); \
    } while (0)

#define LOGT(logger_instance, ...) KIO_LOG_TRACE(logger_instance, __VA_ARGS__)
#define LOGD(logger_instance, ...) KIO_LOG_DEBUG(logger_instance, __VA_ARGS__)
#define LOGI(logger_instance, ...) KIO_LOG_INFO(logger_instance, __VA_ARGS__)
#define LOGW(logger_instance, ...) KIO_LOG_WARN(logger_instance, __VA_ARGS__)
#define LOGE(logger_instance, ...) KIO_LOG_ERROR(logger_instance, __VA_ARGS__)
}  // namespace kio

/* ──────────────────────────────────────────────────────────────
 * Global logging API (like spdlog::info / glog::INFO)
 * Namespace: kio::alog
 * ────────────────────────────────────────────────────────────── */
namespace kio::alog
{
constexpr size_t kDefaultLoggerQueueSize = 1024;

struct Config
{
    size_t queue_size = kDefaultLoggerQueueSize;
    LogLevel level = LogLevel::kInfo;
    std::ostream* output = &std::cout;
};

inline std::unique_ptr<Logger>& GlobalLoggerPtr()
{
    static std::unique_ptr<Logger> instance = nullptr;
    return instance;
}

inline Logger& Get()
{
    // Get the pointer set by configure()
    if (const auto& ptr = GlobalLoggerPtr())
    {
        // If configure() was called, return its logger
        return *ptr;
    }

    // Otherwise, if configure() was never called, create
    // and return a default static instance.
    static Logger default_instance(kDefaultLoggerQueueSize, LogLevel::kInfo, std::cout);
    return default_instance;
}

inline void Configure(size_t queue_size = kDefaultLoggerQueueSize, LogLevel level = LogLevel::kInfo,
                      std::ostream& os = std::cout)
{
    auto& ptr = GlobalLoggerPtr();
    ptr = std::make_unique<Logger>(queue_size, level, os);
}

template <typename... Args>
void Trace(const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args)
{
    Get().Trace(loc, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Debug(const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args)
{
    Get().Debug(loc, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Info(const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args)
{
    Get().Info(loc, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Warn(const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args)
{
    Get().Warn(loc, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Error(const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args)
{
    Get().Error(loc, fmt, std::forward<Args>(args)...);
}

// uses the macros, they are more convenient
#define ALOG_TRACE(...) ::kio::alog::Trace(std::source_location::current(), __VA_ARGS__)
#define ALOG_DEBUG(...) ::kio::alog::Debug(std::source_location::current(), __VA_ARGS__)
#define ALOG_INFO(...)  ::kio::alog::Info(std::source_location::current(), __VA_ARGS__)
#define ALOG_WARN(...)  ::kio::alog::Warn(std::source_location::current(), __VA_ARGS__)
#define ALOG_ERROR(...) ::kio::alog::Error(std::source_location::current(), __VA_ARGS__)
}  // namespace kio::alog

#endif  // KIO_ASYNC_LOGGER_H
