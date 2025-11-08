// Created by Yao ACHI on 25/10/2025.

#ifndef KIO_ASYNC_LOGGER_H
#define KIO_ASYNC_LOGGER_H

#include <chrono>
#include <syncstream>
#include <cstring>
#include <array>
#include <span>
#include <string>
#include <format>
#include <string_view>
#include <memory>
#include <cerrno>
#include <iostream>
#include <unistd.h>
#include <thread>
#include <source_location>
#include <sys/eventfd.h>
#include <functional>

#include "core/include/ds/mpsc_queue.h"

namespace kio {
    inline int64_t get_thread_id() noexcept {
        thread_local const int64_t tid = syscall(SYS_gettid);
        return tid;
    }

    enum class LogLevel: uint8_t {
        Disabled = 0,
        Trace,
        Debug,
        Info,
        Warn,
        Error,
    };

    struct LogMessage {
        static constexpr size_t MSG_CAPACITY = 256;
        std::chrono::system_clock::time_point timestamp{};
        LogLevel level{LogLevel::Info};
        std::string_view file{};
        std::string_view function{};
        uint64_t line{};
        size_t thread_id{};

        std::array<char, MSG_CAPACITY> buffer{};
        std::unique_ptr<std::string> heap_msg{};
        std::span<const char> msg{};

        void set_small_message(std::span<const char> data) {
            std::memcpy(buffer.data(), data.data(), data.size());
            msg = std::span<const char>(buffer.data(), data.size());
            heap_msg.reset();
        }

        void set_large_message(std::string &&s) {
            heap_msg = std::make_unique<std::string>(std::move(s));
            msg = std::span<const char>(heap_msg->data(), heap_msg->size());
        }

        [[nodiscard]]
        std::string_view view() const noexcept {
            return {msg.data(), msg.size()};
        }

        LogMessage(const LogMessage &) = delete;

        LogMessage &operator=(const LogMessage &) = delete;

        LogMessage() = default;

        LogMessage(LogMessage &&other) noexcept {
            timestamp = other.timestamp;
            level = other.level;
            file = other.file;
            function = other.function;
            line = other.line;
            thread_id = other.thread_id;

            if (other.heap_msg) {
                heap_msg = std::move(other.heap_msg);
                msg = std::span<const char>(heap_msg->data(), heap_msg->size());
            } else {
                buffer = other.buffer;
                msg = std::span<const char>(buffer.data(), other.msg.size());
            }
            other.msg = std::span<const char>();
        }

        LogMessage &operator=(LogMessage &&other) noexcept {
            if (this == &other) return *this;
            timestamp = other.timestamp;
            level = other.level;
            file = other.file;
            function = other.function;
            line = other.line;
            thread_id = other.thread_id;

            if (other.heap_msg) {
                heap_msg = std::move(other.heap_msg);
                msg = std::span<const char>(heap_msg->data(), heap_msg->size());
            } else {
                buffer = other.buffer;
                msg = std::span<const char>(buffer.data(), other.msg.size());
                heap_msg.reset();
            }

            other.msg = std::span<const char>();
            return *this;
        }
    };


    class Logger {
    public:
        explicit Logger(const size_t queue_size = 1024, const LogLevel level = LogLevel::Info,
                        std::ostream &output_stream = std::cout)
            : queue_(queue_size),
              level_(level),
              output_stream_(output_stream),
              use_color_(false) {
            wakeup_fd_ = eventfd(0, EFD_CLOEXEC);
            if (wakeup_fd_ < 0) {
                std::osyncstream(std::cerr) << "Failed to create logger eventfd: " << strerror(errno) << std::endl;
            }

            if (&output_stream_ == &std::cout) use_color_ = ::isatty(STDOUT_FILENO) != 0;

            consumer_thread_ = std::jthread([this](const std::stop_token &st) { consume_loop(st); });
        }

        ~Logger() {
            consumer_thread_.request_stop();

            if (wakeup_fd_ >= 0) {
                constexpr uint64_t val = 1;
                (void) ::write(wakeup_fd_, &val, sizeof(val));
            }

            if (consumer_thread_.joinable()) consumer_thread_.join();

            if (wakeup_fd_ >= 0) {
                ::close(wakeup_fd_);
                wakeup_fd_ = -1;
            }
        }

        void set_level(LogLevel level) { level_.store(level, std::memory_order_relaxed); }

        [[nodiscard]]
        bool should_log(LogLevel lvl) const noexcept {
            if (const auto current_level = level_.load(std::memory_order_relaxed); current_level == LogLevel::Disabled) return false;
            return static_cast<int>(lvl) >= static_cast<int>(level_.load(std::memory_order_relaxed));
        }

        template<typename... Args>
        void trace(const std::source_location &loc, std::format_string<Args...> fmt, Args &&... args) {
            log<LogLevel::Trace>(loc, fmt, std::forward<Args>(args)...);
        }

        template<typename... Args>
        void debug(const std::source_location &loc, std::format_string<Args...> fmt, Args &&... args) {
            log<LogLevel::Debug>(loc, fmt, std::forward<Args>(args)...);
        }

        template<typename... Args>
        void info(const std::source_location &loc, std::format_string<Args...> fmt, Args &&... args) {
            log<LogLevel::Info>(loc, fmt, std::forward<Args>(args)...);
        }

        template<typename... Args>
        void warn(const std::source_location &loc, std::format_string<Args...> fmt, Args &&... args) {
            log<LogLevel::Warn>(loc, fmt, std::forward<Args>(args)...);
        }

        template<typename... Args>
        void error(const std::source_location &loc, std::format_string<Args...> fmt, Args &&... args) {
            log<LogLevel::Error>(loc, fmt, std::forward<Args>(args)...);
        }

        void flush() {
            LogMessage entry;
            while (queue_.try_pop(entry)) {
                write(entry);
            }
        }

    private:
        template<LogLevel L, typename... Args>
        void log(const std::source_location &loc, std::format_string<Args...> fmt, Args &&... args) {
            if constexpr (L == LogLevel::Disabled) return;
            if (!should_log(L)) return;

            std::array<char, LogMessage::MSG_CAPACITY> stack_buf{};

            try {
                auto result = std::format_to_n(stack_buf.data(), stack_buf.size(), fmt, std::forward<Args>(args)...);
                const auto written = static_cast<size_t>(result.out - stack_buf.data());

                LogMessage entry{};
                entry.timestamp = std::chrono::system_clock::now();
                entry.level = L;
                entry.file = loc.file_name();
                entry.function = loc.function_name();
                entry.line = loc.line();
                entry.thread_id = get_thread_id();

                if (result >=0 && static_cast<size_t>(result.size) <= LogMessage::MSG_CAPACITY) {
                    entry.set_small_message(std::span<const char>(stack_buf.data(), written));
                } else {
                    std::string large = std::format(fmt, std::forward<Args>(args)...);
                    entry.set_large_message(std::move(large));
                }

                if (queue_.try_push(std::move(entry))) {
                    if (wakeup_fd_ >= 0) {
                        constexpr uint64_t val = 1;
                        if (const ssize_t ret = ::write(wakeup_fd_, &val, sizeof(val)); ret < 0 && errno != EAGAIN) {
                            std::osyncstream(std::cerr) << "Logger wakeup write() error: " << strerror(errno) <<
                                    std::endl;
                        }
                    }
                } else {
                    std::osyncstream(std::cerr) << "Logger queue full. Dropping message.\n";
                }
            } catch (const std::exception &e) {
                std::string fallback = std::string("<log format error: ") + e.what() + ">";
                LogMessage entry{};
                entry.timestamp = std::chrono::system_clock::now();
                entry.level = L;
                entry.file = loc.file_name();
                entry.function = loc.function_name();
                entry.line = loc.line();
                entry.thread_id = get_thread_id();
                if (fallback.size() <= LogMessage::MSG_CAPACITY) {
                    entry.set_small_message(std::span<const char>(fallback.data(), fallback.size()));
                } else {
                    entry.set_large_message(std::move(fallback));
                }
                (void) queue_.try_push(std::move(entry));
            }
        }

        void consume_loop(const std::stop_token &st) {
            uint64_t val;
            while (!st.stop_requested()) {
                if (wakeup_fd_ < 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                } else {
                    if (const ssize_t ret = ::read(wakeup_fd_, &val, sizeof(val)); ret < 0) {
                        if (errno == EINTR) continue;
                        std::osyncstream(std::cerr) << "Logger wakeup read() error: " << strerror(errno) << std::endl;
                        break;
                    }
                }
                flush();
            }
            flush();
        }

        void write(const LogMessage &msg) const {
            const char *color = level_color(msg.level);
            const char *reset = use_color_ ? "\033[0m" : "";

            std::string time_str;
            try {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    msg.timestamp.time_since_epoch()) % 1000;
                time_str = std::format("{:%Y-%m-%d %H:%M:%S}.{:03d}", msg.timestamp, ms.count());
            } catch (...) {
                time_str = R"(????-??-?? ??:??:??.???)";
            }

            std::string_view filename = msg.file;
            if (const auto pos = filename.find_last_of("/\\"); pos != std::string_view::npos) {
                filename = filename.substr(pos + 1);
            }

            output_stream_ << std::format("{}[{}] [{:<5}] [tid:{}] {}:{} - {}{}\n",
                                          color,
                                          time_str,
                                          level_name(msg.level),
                                          msg.thread_id,
                                          filename,
                                          msg.line,
                                          msg.view(),
                                          reset);
        }

        static const char *level_name(const LogLevel lvl) {
            switch (lvl) {
                case LogLevel::Trace: return "TRACE";
                case LogLevel::Debug: return "DEBUG";
                case LogLevel::Info: return "INFO";
                case LogLevel::Warn: return "WARN";
                case LogLevel::Error: return "ERROR";
                default: return "DISA";
            }
        }

        const char *level_color(const LogLevel lvl) const noexcept {
            if (!use_color_) return "";
            switch (lvl) {
                case LogLevel::Trace: return "\033[37m";
                case LogLevel::Debug: return "\033[36m";
                case LogLevel::Info: return "\033[32m";
                case LogLevel::Warn: return "\033[33m";
                case LogLevel::Error: return "\033[31m";
                default: return "\033[0m";
            }
        }

        MPSCQueue<LogMessage> queue_;
        std::atomic<LogLevel> level_;
        std::jthread consumer_thread_;
        int wakeup_fd_{-1};
        std::ostream &output_stream_;
        bool use_color_;
    };

    /* ──────────────── Core macros (kept as-is) ──────────────── */
#define KIO_LOG_TRACE(logger_instance, ...)                                          \
    do {                                                                             \
        if ((logger_instance).should_log(::kio::LogLevel::Trace))                    \
            (logger_instance).trace(std::source_location::current(), __VA_ARGS__);   \
    } while (0)

#define KIO_LOG_DEBUG(logger_instance, ...)                                          \
    do {                                                                             \
        if ((logger_instance).should_log(::kio::LogLevel::Debug))                    \
            (logger_instance).debug(std::source_location::current(), __VA_ARGS__);   \
    } while (0)

#define KIO_LOG_INFO(logger_instance, ...)                                           \
    do {                                                                             \
        if ((logger_instance).should_log(::kio::LogLevel::Info))                     \
            (logger_instance).info(std::source_location::current(), __VA_ARGS__);    \
    } while (0)

#define KIO_LOG_WARN(logger_instance, ...)                                           \
    do {                                                                             \
        if ((logger_instance).should_log(::kio::LogLevel::Warn))                     \
            (logger_instance).warn(std::source_location::current(), __VA_ARGS__);    \
    } while (0)

#define KIO_LOG_ERROR(logger_instance, ...)                                          \
    do {                                                                             \
        if ((logger_instance).should_log(::kio::LogLevel::Error))                    \
            (logger_instance).error(std::source_location::current(), __VA_ARGS__);   \
    } while (0)

#define LOGT(logger_instance, ...) KIO_LOG_TRACE(logger_instance, __VA_ARGS__)
#define LOGD(logger_instance, ...) KIO_LOG_DEBUG(logger_instance, __VA_ARGS__)
#define LOGI(logger_instance, ...) KIO_LOG_INFO(logger_instance, __VA_ARGS__)
#define LOGW(logger_instance, ...) KIO_LOG_WARN(logger_instance, __VA_ARGS__)
#define LOGE(logger_instance, ...) KIO_LOG_ERROR(logger_instance, __VA_ARGS__)
} // namespace kio


/* ──────────────────────────────────────────────────────────────
 * Global logging API (like spdlog::info / glog::INFO)
 * Namespace: kio::alog
 * ────────────────────────────────────────────────────────────── */
namespace kio::alog {
    struct Config {
        size_t queue_size = 1024;
        LogLevel level = LogLevel::Info;
        std::ostream *output = &std::cout;
    };

    inline std::unique_ptr<Logger> &global_logger_ptr() {
        static std::unique_ptr<Logger> instance = nullptr;
        return instance;
    }

    inline Logger &get() {
        // Get the pointer set by configure()
        if (const auto &ptr = global_logger_ptr()) {
            // If configure() was called, return its logger
            return *ptr;
        }

        // Otherwise, if configure() was never called, create
        // and return a default static instance.
        static Logger default_instance(1024, LogLevel::Info, std::cout);
        return default_instance;
    }

    inline void configure(size_t queue_size = 1024,
                          LogLevel level = LogLevel::Info,
                          std::ostream &os = std::cout) {
        auto &ptr = global_logger_ptr();
        ptr = std::make_unique<Logger>(queue_size, level, os);
    }

    template<typename... Args>
    void trace(const std::source_location &loc,
               std::format_string<Args...> fmt,
               Args &&... args) {
        get().trace(loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void debug(const std::source_location &loc,
               std::format_string<Args...> fmt,
               Args &&... args) {
        get().debug(loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(const std::source_location &loc,
              std::format_string<Args...> fmt,
              Args &&... args) {
        get().info(loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warn(const std::source_location &loc,
              std::format_string<Args...> fmt,
              Args &&... args) {
        get().warn(loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(const std::source_location &loc,
               std::format_string<Args...> fmt,
               Args &&... args) {
        get().error(loc, fmt, std::forward<Args>(args)...);
    }

    // uses the macros, they are more convenient
#define ALOG_TRACE(...) ::kio::alog::trace(std::source_location::current(), __VA_ARGS__)
#define ALOG_DEBUG(...) ::kio::alog::debug(std::source_location::current(), __VA_ARGS__)
#define ALOG_INFO(...)  ::kio::alog::info(std::source_location::current(), __VA_ARGS__)
#define ALOG_WARN(...)  ::kio::alog::warn(std::source_location::current(), __VA_ARGS__)
#define ALOG_ERROR(...) ::kio::alog::error(std::source_location::current(), __VA_ARGS__)
}

#endif // KIO_ASYNC_LOGGER_H
