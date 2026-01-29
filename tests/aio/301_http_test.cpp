#include <optional>
#include <string_view>
#include <vector>

#include "aio/http.hpp"
#include <gtest/gtest.h>

using namespace aio::http;

// -----------------------------------------------------------------------------
// Helper Tests
// -----------------------------------------------------------------------------

TEST(HttpProxyTest, CaseHeaderName)
{
    const Header h{"Content-Type", "text/html"};

    // Case-insensitive match
    EXPECT_TRUE(h.NameEquals("Content-Type"));
    EXPECT_TRUE(h.NameEquals("content-type"));
    EXPECT_TRUE(h.NameEquals("CONTENT-TYPE"));
    EXPECT_TRUE(h.NameEquals("CoNtEnT-TyPe"));

    // Mismatch
    EXPECT_FALSE(h.NameEquals("Content-Length"));
    EXPECT_FALSE(h.NameEquals("Content-Type2"));
}

// -----------------------------------------------------------------------------
// Request Header Tests
// -----------------------------------------------------------------------------

TEST(HttpProxyTest, RequestHeaderAccess)
{
    RequestHeader req;
    req.method = "GET";
    std::string path_str = "/api/v1/resource?query=1";
    req.raw_path.assign(path_str.begin(), path_str.end());
    req.version_major = 1;
    req.version_minor = 1;

    req.AppendHeader("Host", "example.com");
    req.AppendHeader("User-Agent", "AIO-Proxy/1.0");
    req.AppendHeader("Accept", "application/json");
    req.AppendHeader("accept", "text/plain");  // Duplicate key, different case

    // Path helpers
    EXPECT_EQ(req.Path(), "/api/v1/resource?query=1");
    EXPECT_EQ(req.PathWithoutQuery(), "/api/v1/resource");
    EXPECT_EQ(req.Query(), "query=1");

    // Header lookup (case-insensitive)
    EXPECT_TRUE(req.HasHeader("host"));
    EXPECT_EQ(req.GetHeader("HOST"), "example.com");
    EXPECT_EQ(req.GetHeader("user-agent"), "AIO-Proxy/1.0");

    // Multi-value headers
    auto accepts = req.GetAllHeaders("accept");
    ASSERT_EQ(accepts.size(), 2);
    EXPECT_EQ(accepts[0], "application/json");
    EXPECT_EQ(accepts[1], "text/plain");

    // Mutation
    req.SetHeader("Host", "new.com");  // Should replace existing
    EXPECT_EQ(req.GetHeader("host"), "new.com");
    EXPECT_EQ(req.GetAllHeaders("host").size(), 1);

    req.RemoveHeader("User-Agent");
    EXPECT_FALSE(req.HasHeader("User-Agent"));
}

TEST(HttpProxyTest, RequestSerialization)
{
    RequestHeader req;
    req.method = "POST";
    std::string path = "/submit";
    req.raw_path.assign(path.begin(), path.end());
    req.version_minor = 1;

    req.AppendHeader("Host", "api.local");
    req.AppendHeader("Content-Type", "application/json");
    req.AppendHeader("Content-Length", "15");

    std::string raw = req.Serialize();

    std::string expected =
        "POST /submit HTTP/1.1\r\n"
        "Host: api.local\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 15\r\n"
        "\r\n";

    EXPECT_EQ(raw, expected);
}

TEST(HttpProxyTest, ComplexValues)
{
    RequestHeader req;
    // Chunked with spaces and mixed case
    req.SetHeader("Transfer-Encoding", " gzip, chunked ");
    EXPECT_TRUE(req.IsChunked());

    req.SetHeader("Transfer-Encoding", "identity");
    EXPECT_FALSE(req.IsChunked());

    // Invalid Content-Length
    req.SetHeader("Content-Length", "abc");
    EXPECT_FALSE(req.ContentLength().has_value());

    // std::from_chars is strict, ensuring "123 " fails if not trimmed by logic before calling it.
    // The implementation in proxy.hpp passes the full string view to from_chars and checks
    // that ptr == end. So trailing spaces cause failure, which is correct/strict.
    req.SetHeader("Content-Length", "123 ");
    EXPECT_FALSE(req.ContentLength().has_value());

    req.SetHeader("Content-Length", "123");
    ASSERT_TRUE(req.ContentLength().has_value());
    EXPECT_EQ(*req.ContentLength(), 123);
}

// -----------------------------------------------------------------------------
// Response Header Tests
// -----------------------------------------------------------------------------

TEST(HttpProxyTest, ResponseHeaderFraming_ContentLength)
{
    ResponseHeader resp;
    resp.version_minor = 1;
    resp.SetHeader("Content-Length", "42");

    auto cl = resp.ContentLength();
    ASSERT_TRUE(cl.has_value());
    EXPECT_EQ(*cl, 42);
    EXPECT_FALSE(resp.IsChunked());
    EXPECT_TRUE(resp.IsKeepAlive());  // HTTP/1.1 default
    EXPECT_TRUE(resp.HasBody());
}

TEST(HttpProxyTest, ResponseHeaderFraming_Chunked)
{
    ResponseHeader resp;
    resp.version_minor = 1;
    resp.SetHeader("Transfer-Encoding", "chunked");

    EXPECT_TRUE(resp.IsChunked());

    // If both CL and Chunked exist, Chunked wins in logic
    resp.SetHeader("Content-Length", "100");
    EXPECT_TRUE(resp.IsChunked());
    // Note: ContentLength() simply parses the header if present.
    // Logic using it should prefer IsChunked().
    EXPECT_TRUE(resp.ContentLength().has_value());
}

TEST(HttpProxyTest, ResponseHeaderFraming_KeepAlive)
{
    ResponseHeader resp;
    resp.version_minor = 1;  // 1.1
    EXPECT_TRUE(resp.IsKeepAlive());

    resp.SetHeader("Connection", "close");
    EXPECT_FALSE(resp.IsKeepAlive());

    resp.SetHeader("Connection", "keep-alive");
    EXPECT_TRUE(resp.IsKeepAlive());

    resp.version_minor = 0;  // 1.0
    resp.RemoveHeader("Connection");
    EXPECT_FALSE(resp.IsKeepAlive());  // 1.0 default close

    resp.SetHeader("Connection", "keep-alive");
    EXPECT_TRUE(resp.IsKeepAlive());
}

TEST(HttpProxyTest, ResponseHeaderFraming_NoBody)
{
    ResponseHeader resp;

    resp.status_code = 204;
    EXPECT_FALSE(resp.HasBody());

    resp.status_code = 304;
    EXPECT_FALSE(resp.HasBody());

    resp.status_code = 200;
    EXPECT_TRUE(resp.HasBody());

    resp.status_code = 101;  // Switching Protocols
    EXPECT_FALSE(resp.HasBody());
}

// -----------------------------------------------------------------------------
// Main Entry Point
// -----------------------------------------------------------------------------

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}