// Tracer is used during dev, not a real production tracing tool
#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <iostream>
#include <source_location>

#include "operation.hpp"

namespace URing
{

enum class TraceEvent : std::uint8_t
{
    Submit,
    Complete,
    Cancel,
    Wake,
    Spawn,
    Despawn
};

struct TraceRecord
{
    std::source_location loc;
    std::chrono::steady_clock::time_point ts;
    Token token{};
    std::int32_t res{};
    TraceEvent event{};
};

#ifndef URING_ENABLE_TRACING
    #define URING_ENABLE_TRACING 0
#endif

template <bool Enabled>
struct TracerImpl
{
    static void submit(Token, std::source_location = std::source_location::current()) noexcept {}
    static void complete(Token, std::int32_t, std::source_location = std::source_location::current()) noexcept {}
    static void cancel(Token, std::source_location = std::source_location::current()) noexcept {}
    static void wake(std::source_location = std::source_location::current()) noexcept {}
    static void flush() noexcept {}
};

template <>
struct TracerImpl<true>
{
    static constexpr std::size_t kCapacity = 16384;

    // Non-atomic head — this is thread-local, no contention possible
    static inline thread_local std::size_t head = 0;
    static inline thread_local std::array<TraceRecord, kCapacity> buf{};

    static void record(TraceEvent e, Token t, std::int32_t res, std::source_location loc) noexcept
    {
        buf[head % kCapacity] = {loc, std::chrono::steady_clock::now(), t, res, e};
        ++head;
    }

    static void submit(Token t, std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::Submit, t, 0, loc);
    }
    static void complete(Token t, std::int32_t res, std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::Complete, t, res, loc);
    }
    static void cancel(Token t, std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::Cancel, t, 0, loc);
    }
    static void wake(std::source_location loc = std::source_location::current()) noexcept
    {
        record(TraceEvent::Wake, {}, 0, loc);
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
                "[{:>10}μs] {:>8} tok={:08x}:{:08x} res={:>6} {}:{}\n", us,
                [](TraceEvent e) -> std::string_view
                {
                    switch (e)
                    {
                        case TraceEvent::Submit:
                            return "SUBMIT";
                        case TraceEvent::Complete:
                            return "COMPLETE";
                        case TraceEvent::Cancel:
                            return "CANCEL";
                        case TraceEvent::Wake:
                            return "WAKE";
                        case TraceEvent::Spawn:
                            return "SPAWN";
                        case TraceEvent::Despawn:
                            return "DESPAWN";
                    }
                    return "?";
                }(r.event),
                r.token.idx, r.token.gen, r.res, r.loc.file_name(), r.loc.line());
        }
        head = 0;
    }
};

using Tracer = TracerImpl<URING_ENABLE_TRACING>;

}  // namespace URing