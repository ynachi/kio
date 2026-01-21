#pragma once
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <source_location>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <sys/eventfd.h>

#ifndef LOG_BUILD_LEVEL
#define LOG_BUILD_LEVEL 0
#endif

namespace aio::alog {

enum class Level : uint8_t {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
  Fatal = 4,
  Disabled = 5
};

inline std::atomic<Level> g_level{Level::Info};
inline std::atomic<bool> g_colors{true};

constexpr Level kBuildMinLevel = static_cast<Level>(LOG_BUILD_LEVEL);

// ---- fixed parameters ----
constexpr size_t kMaxThreads = 64;  // max producer threads
constexpr size_t kQueueSize = 1024; // per-thread records (power of 2)
constexpr size_t kMsgMax = 512;     // bytes per record (incl '\n')

// ---- helpers ----
namespace detail {

struct LevelConfig {
  const char *label;
  const char *color;
};
constexpr LevelConfig kCfg[] = {{"DBG", "\033[36m"},
                                {"INF", "\033[32m"},
                                {"WRN", "\033[33m"},
                                {"ERR", "\033[31m"},
                                {"FTL", "\033[35m"}};
constexpr const char *kReset = "\033[0m";

inline const char *basename(const char *path) {
  const char *slash = std::strrchr(path, '/');
  return slash ? slash + 1 : path;
}

inline uint32_t tid_u32() {
  return static_cast<uint32_t>(::syscall(SYS_gettid));
}

struct Record {
  uint16_t len; // bytes used in msg[]
  uint8_t level;
  uint8_t reserved;
  char msg[kMsgMax];
};

// Single-producer single-consumer ring buffer (power-of-2 capacity).
struct SPSC {
  static constexpr size_t Mask = kQueueSize - 1;
  static_assert((kQueueSize & (kQueueSize - 1)) == 0,
                "kQueueSize must be power of 2");

  std::atomic<uint32_t> head{0}; // producer writes
  std::atomic<uint32_t> tail{0}; // consumer reads
  Record buf[kQueueSize];

  bool try_push(const Record &r) noexcept {
    uint32_t h = head.load(std::memory_order_relaxed);
    uint32_t t = tail.load(std::memory_order_acquire);
    if ((h - t) == kQueueSize)
      return false; // full
    buf[h & Mask] = r;
    head.store(h + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(Record &out) noexcept {
    uint32_t t = tail.load(std::memory_order_relaxed);
    uint32_t h = head.load(std::memory_order_acquire);
    if (t == h)
      return false; // empty
    out = buf[t & Mask];
    tail.store(t + 1, std::memory_order_release);
    return true;
  }
};

struct ThreadSlot {
  std::atomic<bool> active{false};
  SPSC q;
};

inline ThreadSlot g_slots[kMaxThreads];
inline std::atomic<uint32_t> g_next_slot{0};

inline int g_wake_fd = -1;
inline std::atomic<bool> g_running{false};
inline std::jthread g_thread;
inline std::atomic<uint64_t> g_dropped{0};

inline uint32_t register_thread() {
  static thread_local uint32_t slot = UINT32_MAX;
  if (slot != UINT32_MAX)
    return slot;

  uint32_t i = g_next_slot.fetch_add(1, std::memory_order_relaxed);
  if (i >= kMaxThreads) {
    // no slot; logging will drop
    slot = UINT32_MAX;
    return slot;
  }
  g_slots[i].active.store(true, std::memory_order_release);
  slot = i;
  return slot;
}

inline void wake_logger() noexcept {
  int fd = g_wake_fd;
  if (fd < 0)
    return;
  uint64_t one = 1;
  // best-effort; ignore EAGAIN
  (void)::write(fd, &one, sizeof(one));
}

inline void drain_to_fd(int out_fd) {
  Record r{};
  // Drain all slots.
  for (auto & g_slot : g_slots) {
    if (!g_slot.active.load(std::memory_order_acquire))
      continue;
    auto &q = g_slot.q;
    while (q.try_pop(r)) {
      (void)::write(out_fd, r.msg, r.len);
    }
  }
}

inline void logger_loop(int out_fd) {
  // Block on eventfd, drain on wake.
  while (g_running.load(std::memory_order_acquire)) {
    uint64_t n = 0;
    int rc = ::read(g_wake_fd, &n, sizeof(n));
    if (rc < 0 && errno == EINTR)
      continue;
    // Even on error, try to drain.
    drain_to_fd(out_fd);
  }
  // final drain
  drain_to_fd(out_fd);
}

template <typename... Args>
inline void format_record(Record &r, Level lvl, std::source_location loc,
                          std::format_string<Args...> fmt,
                          Args &&...args) noexcept {
  r.level = static_cast<uint8_t>(lvl);

  const bool colors = g_colors.load(std::memory_order_relaxed);
  const auto &cfg = kCfg[static_cast<int>(lvl)];
  const uint32_t tid = tid_u32();

  // Timestamp (cheap-ish): milliseconds since epoch. You can swap this for
  // coarse clock or omit.
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::floor<std::chrono::milliseconds>(now);

  // Build message into r.msg
  char *p = r.msg;
  char *end = r.msg + kMsgMax - 2; // leave room for '\n' and '\0'

  std::format_to_n_result<char *> res{};
  if (colors) {
    res = std::format_to_n(p, end - p, "{}[{}] [{:%T}] [{}] {}:{} | ",
                           cfg.color, cfg.label, ms, tid,
                           basename(loc.file_name()), loc.line());
  } else {
    res = std::format_to_n(p, end - p, "[{}] [{:%T}] [{}] {}:{} | ", cfg.label,
                           ms, tid, basename(loc.file_name()), loc.line());
  }
  p = res.out;

  if (p < end) {
    res = std::format_to_n(p, end - p, fmt, std::forward<Args>(args)...);
    p = res.out;
  }

  if (colors && p < end) {
    res = std::format_to_n(p, end - p, "{}", kReset);
    p = res.out;
  }

  if (p < end)
    *p++ = '\n';
  r.len = static_cast<uint16_t>(p - r.msg);
}

} // namespace detail

// ---- lifecycle ----

// Call once (e.g. at process startup). out_fd defaults to STDERR.
inline void start(int out_fd = STDERR_FILENO) {
  bool expected = false;
  if (!detail::g_running.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel))
    return;

  int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd < 0) {
    // if eventfd fails, still run but will spin less nicely; simplest is to
    // abort start
    detail::g_running.store(false, std::memory_order_release);
    throw std::system_error(errno, std::system_category(), "eventfd");
  }
  detail::g_wake_fd = fd;
  detail::g_thread = std::jthread([out_fd] { detail::logger_loop(out_fd); });
}

inline void stop() {
  if (!detail::g_running.exchange(false, std::memory_order_acq_rel))
    return;
  detail::wake_logger();
  if (detail::g_thread.joinable())
    detail::g_thread.join();
  if (detail::g_wake_fd >= 0)
    ::close(detail::g_wake_fd);
  detail::g_wake_fd = -1;
}

inline uint64_t dropped_count() {
  return detail::g_dropped.load(std::memory_order_relaxed);
}

// ---- logging API ----

template <Level L, typename... Args>
void log(std::source_location loc,
                std::format_string<Args...> fmt,
                Args&&... args) {
  if constexpr (kBuildMinLevel <= L) {
    if (g_level.load(std::memory_order_relaxed) > L) return;

    uint32_t slot = detail::register_thread();
    if (slot == UINT32_MAX) {
      detail::g_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    detail::Record r{};
    detail::format_record(r, L, loc, fmt, std::forward<Args>(args)...);

    if (!detail::g_slots[slot].q.try_push(r)) {
      detail::g_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    detail::wake_logger();

    if constexpr (L == Level::Fatal) {
      detail::wake_logger();
      // optional: std::terminate();
    }
  }
}

// Convenience overload: caller doesn't pass loc
template <Level L, typename... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
  log<L>(std::source_location::current(), fmt, std::forward<Args>(args)...);
}

// Public API: no loc param (avoids the pack+loc ambiguity)
template <typename... Args>
void debug(std::format_string<Args...> f, Args&&... a) {
  log<Level::Debug>(std::source_location::current(), f, std::forward<Args>(a)...);
}
template <typename... Args>
void info(std::format_string<Args...> f, Args&&... a) {
  log<Level::Info>(std::source_location::current(), f, std::forward<Args>(a)...);
}
template <typename... Args>
void warn(std::format_string<Args...> f, Args&&... a) {
  log<Level::Warn>(std::source_location::current(), f, std::forward<Args>(a)...);
}
template <typename... Args>
void error(std::format_string<Args...> f, Args&&... a) {
  log<Level::Error>(std::source_location::current(), f, std::forward<Args>(a)...);
}
template <typename... Args>
void fatal(std::format_string<Args...> f, Args&&... a) {
  log<Level::Fatal>(std::source_location::current(), f, std::forward<Args>(a)...);
}


} // namespace aio::alog
