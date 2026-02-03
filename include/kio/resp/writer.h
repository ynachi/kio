#pragma once

#include "kio/resp/parser.h"

#include <array>
#include <charconv>
#include <string_view>

namespace kio::resp
{

class RespWriter
{
public:
    explicit RespWriter(IoBuffer& buffer) : buffer_(buffer) {}

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
        // Use a small local buffer for integer conversion
        std::array<char, 32> buf;
        auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), val);

        buffer_.Append(":");
        buffer_.Append(std::string_view(buf.data(), ptr - buf.data()));
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
    void Commit() const { buffer_.Commit(); }
    void Rollback() const { buffer_.RollbackPending(); }

private:
    void WriteLenPrefix(const char type, const int64_t len) const
    {
        std::array<char, 32> buf;
        buf[0] = type;
        auto [ptr, ec] = std::to_chars(buf.data() + 1, buf.data() + buf.size(), len);

        buffer_.Append(std::string_view(buf.data(), ptr - buf.data()));
        buffer_.Append("\r\n");
    }

    IoBuffer& buffer_;
};

}  // namespace kio::resp