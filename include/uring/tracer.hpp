#pragma once
#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace URing
{
enum class TraceEvent : uint8_t
{
    // SQE/CQE Lifecycle
    sqe_alloc,
    sqe_fast,
    sqe_slow,
    sqe_full,
    sqe_submit,
    cqe_complete,
    cqe_cancel,
    // Coroutine Execution
    coro_resume,
    coro_suspend,
    spawn_fast,
    spawn_slow,
    spawn_full,
    spawn_detach,
    combinator_start,
    combinator_complete,
    // Reactor Loop
    wake_arm,
    wake_read,
    tick_start,
    tick_end,
    // Allocator Events
    alloc_hit,
    alloc_refill,
    alloc_fallback,
    dealloc_return,
    dealloc_fallback,
    // Task/Promise Events
    task_alloc,
    task_free
};

struct TraceRecord
{
    uint64_t timestamp_ns;
    TraceEvent event;
    uint32_t idx;
    uint32_t gen;
    int32_t result;
    void* handle_ptr;
    const char* context;
};

class Tracer
{
public:
    static thread_local std::vector<TraceRecord> tl_buffer;
    static constexpr size_t kBufferCapacity = 16384;

    static void emit(TraceEvent event, uint32_t idx = 0, uint32_t gen = 0, int32_t res = 0,
                     std::coroutine_handle<> h = nullptr, const char* ctx = nullptr) noexcept
    {
        uint64_t ts =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();

        TraceRecord rec{ts, event, idx, gen, res, h.address(), ctx};
        if (tl_buffer.empty())
            tl_buffer.reserve(kBufferCapacity);
        tl_buffer.push_back(rec);

        if (tl_buffer.size() >= kBufferCapacity)
            flush();
    }

    static void flush() noexcept;
    static void dump(std::string_view filename = "uring_trace.json") noexcept;
    static void reset() noexcept;

private:
    static std::mutex s_flush_mutex;
    static std::vector<TraceRecord> s_global_log;
};

inline std::mutex Tracer::s_flush_mutex;
inline std::vector<TraceRecord> Tracer::s_global_log;
inline thread_local std::vector<TraceRecord> Tracer::tl_buffer;

inline void Tracer::flush() noexcept
{
    if (tl_buffer.empty())
        return;
    std::lock_guard<std::mutex> lock(s_flush_mutex);
    s_global_log.insert(s_global_log.end(), std::make_move_iterator(tl_buffer.begin()),
                        std::make_move_iterator(tl_buffer.end()));
    tl_buffer.clear();
}

inline void Tracer::reset() noexcept
{
    std::lock_guard<std::mutex> lock(s_flush_mutex);
    s_global_log.clear();
    tl_buffer.clear();
}

inline void Tracer::dump(std::string_view filename) noexcept
{
    flush();
    if (s_global_log.empty())
        return;

    std::ofstream out(std::string{filename});
    if (!out)
        return;

    out << "[\n";
    for (size_t i = 0; i < s_global_log.size(); ++i)
    {
        const auto& r = s_global_log[i];
        const char* ev_name = "unknown";

        switch (std::to_underlying(r.event))
        {
            case std::to_underlying(TraceEvent::alloc_hit):
                ev_name = "alloc_hit";
                break;
            case std::to_underlying(TraceEvent::alloc_refill):
                ev_name = "alloc_refill";
                break;
            case std::to_underlying(TraceEvent::alloc_fallback):
                ev_name = "alloc_fallback";
                break;
            case std::to_underlying(TraceEvent::dealloc_return):
                ev_name = "dealloc_return";
                break;
            case std::to_underlying(TraceEvent::dealloc_fallback):
                ev_name = "dealloc_fallback";
                break;
            case std::to_underlying(TraceEvent::task_alloc):
                ev_name = "task_alloc";
                break;
            case std::to_underlying(TraceEvent::task_free):
                ev_name = "task_free";
                break;
            case std::to_underlying(TraceEvent::sqe_fast):
                ev_name = "sqe_fast";
                break;
            case std::to_underlying(TraceEvent::sqe_slow):
                ev_name = "sqe_slow";
                break;
            case std::to_underlying(TraceEvent::sqe_full):
                ev_name = "sqe_full";
                break;
            case std::to_underlying(TraceEvent::sqe_submit):
                ev_name = "sqe_submit";
                break;
            case std::to_underlying(TraceEvent::cqe_complete):
                ev_name = "cqe_complete";
                break;
            case std::to_underlying(TraceEvent::coro_resume):
                ev_name = "coro_resume";
                break;
            case std::to_underlying(TraceEvent::tick_start):
                ev_name = "tick_start";
                break;
            case std::to_underlying(TraceEvent::tick_end):
                ev_name = "tick_end";
                break;
            default:
                ev_name = "event";
                break;
        }

        out << "  {\"ph\":\"X\",\"cat\":\"uring\",\"name\":\"" << ev_name << "\",\"ts\":" << (r.timestamp_ns / 1000.0)
            << ",\"dur\":1,\"id\":" << r.idx << ",\"args\":{\"size_bytes\":" << r.result << ",\"ctx\":\""
            << (r.context ? r.context : "") << "\"}}";
        if (i + 1 < s_global_log.size())
            out << ",";
        out << "\n";
    }
    out << "]\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// ZERO-COST MACROS - All pointer-compatible for consistency
// ─────────────────────────────────────────────────────────────────────────────
#ifdef URING_ENABLE_TRACING

    // Op name tracking (for PendingOp*)
    #define URING_TRACE_OP_FIELD const char* _trace_op_name = nullptr;
    #define URING_TRACE_OP_MEMBER URING_TRACE_OP_FIELD
    #define URING_TRACE_OP_RESET(op)            \
        do                                      \
        {                                       \
            if ((op))                           \
                (op)->_trace_op_name = nullptr; \
        } while (0)
    #define URING_TRACE_OP_NAME(op)  ((op) ? (op)->_trace_op_name : nullptr)
    #define URING_TRACE_OP_ARG(name) name,
    #define URING_TRACE_OP_PARAM     , const char* name
    #define URING_TRACE_OP_CTOR_INIT , _trace_op_name(name),
    #define URING_TRACE_SET_OP_NAME(op)             \
        do                                          \
        {                                           \
            if ((op))                               \
                (op)->_trace_op_name = _trace_op_name; \
        } while (0)

    // Allocator macros for CoroAllocator
    #define URING_TRACE_ALLOC_HIT(bucket, sz) \
        URing::Tracer::emit(URing::TraceEvent::alloc_hit, bucket, 0, static_cast<int32_t>(sz))
    #define URING_TRACE_ALLOC_REFILL(bucket, sz) \
        URing::Tracer::emit(URing::TraceEvent::alloc_refill, bucket, 0, static_cast<int32_t>(sz))
    #define URING_TRACE_ALLOC_FALLBACK(bucket, sz) \
        URing::Tracer::emit(URing::TraceEvent::alloc_fallback, bucket, 0, static_cast<int32_t>(sz))
    #define URING_TRACE_DEALLOC_RETURN(bucket, sz) \
        URing::Tracer::emit(URing::TraceEvent::dealloc_return, bucket, 0, static_cast<int32_t>(sz))
    #define URING_TRACE_DEALLOC_FALLBACK(bucket, sz) \
        URing::Tracer::emit(URing::TraceEvent::dealloc_fallback, bucket, 0, static_cast<int32_t>(sz))

    // Task/Promise allocation tracing
    #define URING_TRACE_ALLOC(sz) URing::Tracer::emit(URing::TraceEvent::task_alloc, 0, 0, static_cast<int32_t>(sz))
    #define URING_TRACE_FREE(sz)  URing::Tracer::emit(URing::TraceEvent::task_free, 0, 0, static_cast<int32_t>(sz))

    // SQE/CQE Macros
    #define URING_TRACE_SQE_ALLOC(t)     URing::Tracer::emit(URing::TraceEvent::sqe_alloc, (t).idx, (t).gen)
    #define URING_TRACE_SQE_FAST(t)      URing::Tracer::emit(URing::TraceEvent::sqe_fast, (t).idx, (t).gen)
    #define URING_TRACE_SQE_SLOW(t, ret) URing::Tracer::emit(URing::TraceEvent::sqe_slow, (t).idx, (t).gen, ret)
    #define URING_TRACE_SQE_FULL(t)      URing::Tracer::emit(URing::TraceEvent::sqe_full, (t).idx, (t).gen)
    #define URING_TRACE_SUBMIT(t)        URing::Tracer::emit(URing::TraceEvent::sqe_submit, (t).idx, (t).gen)
    #define URING_TRACE_COMPLETE(t, res, op)                                                                       \
        URing::Tracer::emit(URing::TraceEvent::cqe_complete, (t).idx, (t).gen, res, (op) ? (op)->handle : nullptr, \
                            URING_TRACE_OP_NAME(op))
    #define URING_TRACE_CANCEL(t) URing::Tracer::emit(URing::TraceEvent::cqe_cancel, (t).idx, (t).gen)

    // Coroutine & Reactor Macros
    #define URING_TRACE_WAKE()       URing::Tracer::emit(URing::TraceEvent::wake_read)
    #define URING_TRACE_RESUME(h)    URing::Tracer::emit(URing::TraceEvent::coro_resume, 0, 0, 0, h)
    #define URING_TRACE_SUSPEND(h)   URing::Tracer::emit(URing::TraceEvent::coro_suspend, 0, 0, 0, h)
    #define URING_TRACE_SPAWN_FAST() URing::Tracer::emit(URing::TraceEvent::spawn_fast)
    #define URING_TRACE_SPAWN_SLOW() URing::Tracer::emit(URing::TraceEvent::spawn_slow)
    #define URING_TRACE_SPAWN_FULL() URing::Tracer::emit(URing::TraceEvent::spawn_full)
    #define URING_TRACE_COMBINATOR(name, idx, total) \
        URing::Tracer::emit(URing::TraceEvent::combinator_start, idx, total, 0, nullptr, name)

struct TraceTickScope
{
    TraceTickScope() { URing::Tracer::emit(URing::TraceEvent::tick_start); }
    ~TraceTickScope() { URing::Tracer::emit(URing::TraceEvent::tick_end); }
};

#else
    // Zero-cost disabled state
    #define URING_TRACE_OP_FIELD
    #define URING_TRACE_OP_MEMBER
    #define URING_TRACE_OP_RESET(op) ((void)0)
    #define URING_TRACE_OP_NAME(op)  nullptr
    #define URING_TRACE_OP_ARG(name)
    #define URING_TRACE_OP_PARAM
    #define URING_TRACE_OP_CTOR_INIT ,
    #define URING_TRACE_SET_OP_NAME(op) ((void)0)

    #define URING_TRACE_ALLOC_HIT(b, s)        ((void)0)
    #define URING_TRACE_ALLOC_REFILL(b, s)     ((void)0)
    #define URING_TRACE_ALLOC_FALLBACK(b, s)   ((void)0)
    #define URING_TRACE_DEALLOC_RETURN(b, s)   ((void)0)
    #define URING_TRACE_DEALLOC_FALLBACK(b, s) ((void)0)

    #define URING_TRACE_ALLOC(sz) ((void)0)
    #define URING_TRACE_FREE(sz)  ((void)0)

    #define URING_TRACE_SQE_ALLOC(t)         ((void)0)
    #define URING_TRACE_SQE_FAST(t)          ((void)0)
    #define URING_TRACE_SQE_SLOW(t, ret)     ((void)0)
    #define URING_TRACE_SQE_FULL(t)          ((void)0)
    #define URING_TRACE_SUBMIT(t)            ((void)0)
    #define URING_TRACE_COMPLETE(t, res, op) ((void)0)
    #define URING_TRACE_CANCEL(t)            ((void)0)

    #define URING_TRACE_WAKE()              ((void)0)
    #define URING_TRACE_RESUME(h)           ((void)0)
    #define URING_TRACE_SUSPEND(h)          ((void)0)
    #define URING_TRACE_SPAWN_FAST()        ((void)0)
    #define URING_TRACE_SPAWN_SLOW()        ((void)0)
    #define URING_TRACE_SPAWN_FULL()        ((void)0)
    #define URING_TRACE_COMBINATOR(n, i, t) ((void)0)

struct TraceTickScope
{
};
#endif

}  // namespace URing