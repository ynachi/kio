#pragma once

#include <charconv>
#include <string_view>

#include "aio/io.hpp"

namespace aio::resp
{

class RespWriter
{
public:
    explicit RespWriter(WritevBuffer& buffer) : buffer_(buffer) {}

    // -----------------------------------------------------------------------
    // Primitive: Simple String (+OK\r\n)
    // -----------------------------------------------------------------------
    void WriteSimpleString(std::string_view s) const
    {
        buffer_.Append("+");
        buffer_.Append(s);
        buffer_.Append("\r\n");
    }

    // -----------------------------------------------------------------------
    // Primitive: Error (-ERR Msg\r\n)
    // -----------------------------------------------------------------------
    void WriteError(std::string_view msg) const
    {
        buffer_.Append("-");
        buffer_.Append(msg);
        buffer_.Append("\r\n");
    }

    // -----------------------------------------------------------------------
    // Primitive: Integer (:123\r\n)
    // -----------------------------------------------------------------------
    void WriteInteger(const int64_t val) const
    {
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
        buffer_.Append(":");
        buffer_.Append(std::string_view(buf, ptr - buf));
        buffer_.Append("\r\n");
    }

    // -----------------------------------------------------------------------
    // Primitive: Bulk String ($len\r\nPayload\r\n)
    // -----------------------------------------------------------------------
    void WriteBulkString(std::string_view s) const
    {
        WriteLenPrefix('$', static_cast<int64_t>(s.size()));
        buffer_.Append(s);
        buffer_.Append("\r\n");
    }

    void WriteNullBulk() const { buffer_.Append("$-1\r\n"); }

    // -----------------------------------------------------------------------
    // Primitive: Array (*count\r\n)
    // -----------------------------------------------------------------------
    void WriteArrayHeader(const int64_t count) const { WriteLenPrefix('*', count); }

    void WriteNullArray() const { buffer_.Append("*-1\r\n"); }

    // -----------------------------------------------------------------------
    // Transaction Control
    // -----------------------------------------------------------------------

    // Makes all appended data visible for sending.
    // Call this after building a complete logical response.
    void Commit() const { buffer_.Commit(); }

    // Discards uncommitted data (e.g. if an error occurs during serialization)
    void Rollback() const { buffer_.RollbackPending(); }

private:
    void WriteLenPrefix(const char type, const int64_t len) const
    {
        char buf[32];
        buf[0] = type;
        auto [ptr, ec] = std::to_chars(buf + 1, buf + sizeof(buf), len);
        buffer_.Append(std::string_view(buf, ptr - buf));
        buffer_.Append("\r\n");
    }

    WritevBuffer& buffer_;
};

}  // namespace aio::resp
