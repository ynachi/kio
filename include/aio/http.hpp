//
// Simple http header parser for proxies
//

#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aio/core/core.hpp"
#include "aio/io.hpp"

namespace aio::http
{

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

inline bool IEquals(std::string_view a, std::string_view b)
{
    return std::ranges::equal(
        a, b, [](const char c1, const char c2)
        { return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2)); });
}

// -----------------------------------------------------------------------------
// Case-Preserving Header
// -----------------------------------------------------------------------------

/// A header that preserves the original case of the name.
/// Proxies must not alter header casing for transparency.
struct Header
{
    std::string name;  // Original case: "Content-Type"
    std::string value;

    // Case-insensitive comparison for lookups
    [[nodiscard]] bool NameEquals(std::string_view other) const { return IEquals(name, other); }
};

// -----------------------------------------------------------------------------
// Request Header (for proxying)
// -----------------------------------------------------------------------------

/// HTTP request headers with case preservation and raw path support.
struct RequestHeader
{
    std::string method;          // Original case preserved
    std::vector<char> raw_path;  // Raw bytes, may not be valid UTF-8
    int version_major = 1;
    int version_minor = 1;
    std::vector<Header> headers;

    // --- Path access ---

    [[nodiscard]] std::string_view Path() const { return {raw_path.data(), raw_path.size()}; }

    [[nodiscard]] std::string_view PathWithoutQuery() const
    {
        auto p = Path();
        auto qpos = p.find('?');
        return qpos != std::string_view::npos ? p.substr(0, qpos) : p;
    }

    [[nodiscard]] std::string_view Query() const
    {
        auto p = Path();
        auto qpos = p.find('?');
        return qpos != std::string_view::npos ? p.substr(qpos + 1) : std::string_view{};
    }

    // --- Header access ---

    [[nodiscard]] std::string_view GetHeader(std::string_view name) const
    {
        for (const auto& h : headers)
        {
            if (h.NameEquals(name))
                return h.value;
        }
        return {};
    }

    [[nodiscard]] std::vector<std::string_view> GetAllHeaders(std::string_view name) const
    {
        std::vector<std::string_view> result;
        for (const auto& h : headers)
        {
            if (h.NameEquals(name))
                result.push_back(h.value);
        }
        return result;
    }

    [[nodiscard]] bool HasHeader(std::string_view name) const { return !GetHeader(name).empty(); }

    void RemoveHeader(std::string_view name)
    {
        std::erase_if(headers, [&](const Header& h) { return h.NameEquals(name); });
    }

    void SetHeader(std::string_view name, std::string_view value)
    {
        RemoveHeader(name);
        headers.push_back({std::string(name), std::string(value)});
    }

    void AppendHeader(std::string name, std::string value) { headers.push_back({std::move(name), std::move(value)}); }

    // --- Body framing ---

    [[nodiscard]] std::optional<size_t> ContentLength() const
    {
        auto val = GetHeader("content-length");
        if (val.empty())
            return std::nullopt;

        size_t result = 0;
        auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), result);
        if (ec != std::errc{} || ptr != val.data() + val.size())
            return std::nullopt;
        return result;
    }

    [[nodiscard]] bool IsChunked() const
    {
        auto te = GetHeader("transfer-encoding");
        if (te.empty())
            return false;

        // Find last encoding
        auto last_comma = te.rfind(',');
        auto last = (last_comma != std::string_view::npos) ? te.substr(last_comma + 1) : te;

        // Trim whitespace
        while (!last.empty() && (last.front() == ' ' || last.front() == '\t'))
            last.remove_prefix(1);
        while (!last.empty() && (last.back() == ' ' || last.back() == '\t'))
            last.remove_suffix(1);

        return IEquals(last, "chunked");
    }

    [[nodiscard]] bool IsKeepAlive() const
    {
        auto conn = GetHeader("connection");
        if (version_minor >= 1)
        {
            // HTTP/1.1 defaults to keep-alive unless "close"
            return !IEquals(conn, "close");
        }
        // HTTP/1.0 defaults to close unless "keep-alive"
        return IEquals(conn, "keep-alive");
    }

    // --- Serialization ---

    /// Serialize to wire format, appending to `out`.
    void Serialize(std::string& out) const
    {
        // Request line
        out.append(method);
        out.push_back(' ');
        out.append(raw_path.data(), raw_path.size());
        out.append(" HTTP/");
        out.push_back(static_cast<char>('0' + version_major));
        out.push_back('.');
        out.push_back(static_cast<char>('0' + version_minor));
        out.append("\r\n");

        // Headers (case preserved)
        for (const auto& h : headers)
        {
            out.append(h.name);
            out.append(": ");
            out.append(h.value);
            out.append("\r\n");
        }

        out.append("\r\n");
    }

    [[nodiscard]] std::string Serialize() const
    {
        std::string out;
        out.reserve(512);
        Serialize(out);
        return out;
    }
};

// -----------------------------------------------------------------------------
// Response Header (for proxying)
// -----------------------------------------------------------------------------

/// HTTP response headers with case preservation.
struct ResponseHeader
{
    int status_code = 200;
    std::string reason_phrase;  // Original reason, may be empty
    int version_major = 1;
    int version_minor = 1;
    std::vector<Header> headers;

    // --- Header access (same as RequestHeader) ---

    [[nodiscard]] std::string_view GetHeader(std::string_view name) const
    {
        for (const auto& h : headers)
        {
            if (h.NameEquals(name))
                return h.value;
        }
        return {};
    }

    [[nodiscard]] std::vector<std::string_view> GetAllHeaders(std::string_view name) const
    {
        std::vector<std::string_view> result;
        for (const auto& h : headers)
        {
            if (h.NameEquals(name))
                result.push_back(h.value);
        }
        return result;
    }

    [[nodiscard]] bool HasHeader(std::string_view name) const { return !GetHeader(name).empty(); }

    void RemoveHeader(std::string_view name)
    {
        std::erase_if(headers, [&](const Header& h) { return h.NameEquals(name); });
    }

    void SetHeader(std::string_view name, std::string_view value)
    {
        RemoveHeader(name);
        headers.push_back({std::string(name), std::string(value)});
    }

    void AppendHeader(std::string name, std::string value) { headers.push_back({std::move(name), std::move(value)}); }

    // --- Body framing ---

    [[nodiscard]] std::optional<size_t> ContentLength() const
    {
        auto val = GetHeader("content-length");
        if (val.empty())
            return std::nullopt;

        size_t result = 0;
        auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), result);
        if (ec != std::errc{} || ptr != val.data() + val.size())
            return std::nullopt;
        return result;
    }

    [[nodiscard]] bool IsChunked() const
    {
        auto te = GetHeader("transfer-encoding");
        if (te.empty())
            return false;

        auto last_comma = te.rfind(',');
        auto last = (last_comma != std::string_view::npos) ? te.substr(last_comma + 1) : te;

        while (!last.empty() && (last.front() == ' ' || last.front() == '\t'))
            last.remove_prefix(1);
        while (!last.empty() && (last.back() == ' ' || last.back() == '\t'))
            last.remove_suffix(1);

        return IEquals(last, "chunked");
    }

    [[nodiscard]] bool IsKeepAlive() const
    {
        auto conn = GetHeader("connection");
        if (version_minor >= 1)
        {
            return !IEquals(conn, "close");
        }
        return IEquals(conn, "keep-alive");
    }

    /// Does this response have a body?
    /// 1xx, 204, 304 responses have no body.
    /// HEAD responses have no body (but caller must know the method).
    [[nodiscard]] bool HasBody() const
    {
        if (status_code >= 100 && status_code < 200)
            return false;
        if (status_code == 204 || status_code == 304)
            return false;
        return true;
    }

    // --- Serialization ---

    void Serialize(std::string& out) const
    {
        // Status line
        out.append("HTTP/");
        out.push_back(static_cast<char>('0' + version_major));
        out.push_back('.');
        out.push_back(static_cast<char>('0' + version_minor));
        out.push_back(' ');

        // Status code
        char code_buf[4];
        code_buf[0] = static_cast<char>('0' + (status_code / 100));
        code_buf[1] = static_cast<char>('0' + (status_code / 10) % 10);
        code_buf[2] = static_cast<char>('0' + (status_code % 10));
        code_buf[3] = ' ';
        out.append(code_buf, 4);

        // Reason phrase
        if (!reason_phrase.empty())
        {
            out.append(reason_phrase);
        }
        else
        {
            out.append(DefaultReasonPhrase(status_code));
        }
        out.append("\r\n");

        // Headers
        for (const auto& h : headers)
        {
            out.append(h.name);
            out.append(": ");
            out.append(h.value);
            out.append("\r\n");
        }

        out.append("\r\n");
    }

    [[nodiscard]] std::string Serialize() const
    {
        std::string out;
        out.reserve(512);
        Serialize(out);
        return out;
    }

private:
    static constexpr std::string_view DefaultReasonPhrase(int code)
    {
        switch (code)
        {
            case 200:
                return "OK";
            case 201:
                return "Created";
            case 204:
                return "No Content";
            case 301:
                return "Moved Permanently";
            case 302:
                return "Found";
            case 304:
                return "Not Modified";
            case 400:
                return "Bad Request";
            case 401:
                return "Unauthorized";
            case 403:
                return "Forbidden";
            case 404:
                return "Not Found";
            case 500:
                return "Internal Server Error";
            case 502:
                return "Bad Gateway";
            case 503:
                return "Service Unavailable";
            default:
                return "Unknown";
        }
    }
};

}  // namespace aio::http