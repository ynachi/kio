// Tracer is used during dev, not a real production tracing tool
#pragma once
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <format>
#include <iostream>
#include <source_location>

#include "operation.hpp"
#include "trace_config.hpp"

namespace URing
{

enum class TraceEvent : std::uint8_t
{
    SqeFast,
    SqeSlow,
    SqeFull,
    Submit,
    Complete,
    Cancel,
    Wake,
    SpawnFast,
    SpawnSlow,
    SpawnFull,
    Alloc,
    Free
};

struct TraceRecord
{
    std::source_location loc;
    std::chrono::steady_clock::time_point ts;
    const char* op{};
    Token token{};
    std::int32_t res{};
    TraceEvent event{};
};

template <bool Enabled>
struct TracerImpl
{
    static void submit(Token, const char* = "", std::source_location = std::source_location::current()) noexcept {}
    static void sqe_fast(Token, const char* = "", std::source_location = std::source_location::current()) noexcept {}
    static void sqe_slow(Token, std::int32_t, const char* = "",
                         std::source_location = std::source_location::current()) noexcept
    {
    }
    static void sqe_full(Token, const char* = "", std::source_location = std::source_location::current()) noexcept {}
    static void complete(Token, std::int32_t, const char* = "",
                         std::source_location = std::source_location::current()) noexcept
    {
    }
    static void spawn_fast(std::source_location = std::source_location::current()) noexcept {}
    static void spawn_slow(std::source_location = std::source_location::current()) noexcept {}
    static void spawn_full(std::source_location = std::source_location::current()) noexcept {}
    static void cancel(Token, std::source_location = std::source_location::current()) noexcept {}
    static void wake(std::source_location = std::source_location::current()) noexcept {}
    static void alloc(std::size_t, std::source_location = std::source_location::current()) noexcept {}
    static void free(std::size_t, std::source_location = std::source_location::current()) noexcept {}
    static void flush() noexcept {}
};

template <>
struct TracerImpl<true>
{
    static constexpr std::size_t kCapacity = 16384;

    // Non-atomic head — this is thread-local, no contention possible
    static inline thread_local std::size_t head = 0;
    static inline thread_local std::array<TraceRecord, kCapacity> buf{};

    static void record(TraceEvent e, Token t, std::int32_t res, const char* op, std::source_location loc) noexcept
    {
        buf[head % kCapacity] = {loc, std::chrono::steady_clock::now(), op, t, res, e};
        ++head;
    }

    static void submit(Token t, const char* op = "", std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::Submit, t, 0, op, loc);
    }
    static void sqe_fast(Token t, const char* op = "", std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::SqeFast, t, 0, op, loc);
    }
    static void sqe_slow(Token t, std::int32_t submit_res, const char* op = "",
                         std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::SqeSlow, t, submit_res, op, loc);
    }
    static void sqe_full(Token t, const char* op = "", std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::SqeFull, t, -ENOSPC, op, loc);
    }
    static void complete(Token t, std::int32_t res, const char* op = "",
                         std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::Complete, t, res, op, loc);
    }
    static void cancel(Token t, std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::Cancel, t, 0, "", loc);
    }
    static void spawn_fast(std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::SpawnFast, {}, 0, "spawn", loc);
    }
    static void spawn_slow(std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::SpawnSlow, {}, 0, "spawn", loc);
    }
    static void spawn_full(std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::SpawnFull, {}, 0, "spawn", loc);
    }
    static void wake(std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::Wake, {}, 0, "", loc);
    }
    static void alloc(std::size_t size, std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::Alloc, {}, static_cast<std::int32_t>(size), "coro", loc);
    }
    static void free(std::size_t size, std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::Free, {}, static_cast<std::int32_t>(size), "coro", loc);
    }
    static void flush() noexcept
    {
        const std::size_t count = std::min(head, kCapacity);
        const std::size_t start = head > kCapacity ? head % kCapacity : 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& r = buf[(start + i) % kCapacity];
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(r.ts.time_since_epoch()).count();
            std::cerr << std::format(
                "[{:>10}μs] {:>8} op={:<10} tok={:08x}:{:08x} res={} {}:{}\n", us,
                [](TraceEvent e) -> std::string_view
                {
                    switch (e)
                    {
                        case TraceEvent::SqeFast:
                            return "SQE_FAST";
                        case TraceEvent::SqeSlow:
                            return "SQE_SLOW";
                        case TraceEvent::SqeFull:
                            return "SQE_FULL";
                        case TraceEvent::Submit:
                            return "SUBMIT";
                        case TraceEvent::Complete:
                            return "COMPLETE";
                        case TraceEvent::Cancel:
                            return "CANCEL";
                        case TraceEvent::Wake:
                            return "WAKE";
                        case TraceEvent::SpawnFast:
                            return "SPAWN_F";
                        case TraceEvent::SpawnSlow:
                            return "SPAWN_S";
                        case TraceEvent::SpawnFull:
                            return "SPAWN_X";
                        case TraceEvent::Alloc:
                            return "ALLOC";
                        case TraceEvent::Free:
                            return "FREE";
                    }
                    return "?";
                }(r.event),
                r.op ? r.op : "", r.token.idx, r.token.gen, r.res, r.loc.file_name(), r.loc.line());
        }
        head = 0;
    }
};

using Tracer = TracerImpl<URING_ENABLE_TRACING>;

}  // namespace URing
