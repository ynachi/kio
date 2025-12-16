//
// Created by Yao ACHI on 14/12/2025.
//

#include "kio/resp/parser.h"

#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

using namespace kio::resp;

class ParserTest : public ::testing::Test
{
protected:
    // Initialize with the default config
    Parser parser{ParserConfig{}};

    void feed(std::string_view data) { parser.buffer().extend_from_slice({data.data(), data.size()}); }

    void SetUp() override
    {
        // Ensure a clean state
        parser.buffer().clear();
    }
};


TEST_F(ParserTest, ParseSimpleString)
{
    feed("+OK\r\n");
    const auto result = parser.next_frame();
    ASSERT_TRUE(result.has_value());

    const auto frame = *result;
    EXPECT_EQ(frame.type, FrameType::SimpleString);
    EXPECT_EQ(frame.size, 5);
    EXPECT_EQ(get_simple_payload(frame), "OK");

    parser.consume(frame);
    EXPECT_EQ(parser.buffer().consumed(), 5);
}

TEST_F(ParserTest, ParseInteger)
{
    feed(":1000\r\n:-42\r\n");

    // First integer
    const auto res1 = parser.next_frame();
    ASSERT_TRUE(res1.has_value());
    EXPECT_EQ(res1->type, FrameType::Integer);
    EXPECT_EQ(get_simple_payload(*res1), "1000");
    parser.consume(*res1);

    // Second integer
    const auto res2 = parser.next_frame();
    ASSERT_TRUE(res2.has_value());
    EXPECT_EQ(res2->type, FrameType::Integer);
    EXPECT_EQ(get_simple_payload(*res2), "-42");
    parser.consume(*res2);
}

TEST_F(ParserTest, ParseDouble)
{
    feed(",1000\r\n,-42\r\n,34.5\r\n");

    // First integer
    const auto res1 = parser.next_frame();
    ASSERT_TRUE(res1.has_value());
    EXPECT_EQ(res1->type, FrameType::Double);
    EXPECT_EQ(get_simple_payload(*res1), "1000");
    parser.consume(*res1);

    // Second integer
    const auto res2 = parser.next_frame();
    ASSERT_TRUE(res2.has_value());
    EXPECT_EQ(res2->type, FrameType::Double);
    EXPECT_EQ(get_simple_payload(*res2), "-42");
    parser.consume(*res2);

    // Fird integer
    const auto res3 = parser.next_frame();
    ASSERT_TRUE(res3.has_value());
    EXPECT_EQ(res3->type, FrameType::Double);
    EXPECT_EQ(get_simple_payload(*res3), "34.5");
    parser.consume(*res3);
}

TEST_F(ParserTest, ParseBulkString)
{
    feed("$5\r\nhello\r\n");
    const auto result = parser.next_frame();
    ASSERT_TRUE(result.has_value());

    const auto frame = *result;
    EXPECT_EQ(frame.type, FrameType::BulkString);
    EXPECT_EQ(get_bulk_payload(frame), "hello");
}

TEST_F(ParserTest, ParseEmptyBulkString)
{
    feed("$0\r\n\r\n");
    const auto result = parser.next_frame();
    ASSERT_TRUE(result.has_value());

    const auto frame = *result;
    EXPECT_EQ(frame.type, FrameType::BulkString);
    EXPECT_EQ(get_bulk_payload(frame), "");
    // Size check: $0\r\n (4) + \r\n (2) = 6
    EXPECT_EQ(frame.size, 6);
}

TEST_F(ParserTest, ParseNullBulkString)
{
    feed("$-1\r\n");
    const auto result = parser.next_frame();
    ASSERT_TRUE(result.has_value());

    const auto frame = *result;
    EXPECT_EQ(frame.type, FrameType::BulkString);
    EXPECT_TRUE(get_bulk_payload(frame).empty());
    EXPECT_EQ(frame.size, 5);
}

TEST_F(ParserTest, ParseBoolean)
{
    feed("#t\r\n#f\r\n");

    auto t = parser.next_frame();
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(get_simple_payload(*t), "t");
    parser.consume(*t);

    const auto f = parser.next_frame();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(get_simple_payload(*f), "f");
}


TEST_F(ParserTest, ParseArray)
{
    // Array of 3 items: [1, 2, 3]
    feed("*3\r\n:1\r\n:2\r\n:3\r\n");

    auto result = parser.next_frame();
    ASSERT_TRUE(result.has_value());
    auto frame = *result;

    EXPECT_EQ(frame.type, FrameType::Array);
    EXPECT_EQ(frame.element_count, 3);

    // Verify Iterator
    FrameIterator iter(frame, parser);

    auto child1 = iter.next();
    ASSERT_TRUE(child1);
    EXPECT_EQ(get_simple_payload(*child1), "1");

    auto child2 = iter.next();
    ASSERT_TRUE(child2);
    EXPECT_EQ(get_simple_payload(*child2), "2");

    auto child3 = iter.next();
    ASSERT_TRUE(child3);
    EXPECT_EQ(get_simple_payload(*child3), "3");

    ASSERT_FALSE(iter.next());
}

TEST_F(ParserTest, ParseMap)
{
    // Map with 2 pairs
    feed("%2\r\n+key1\r\n:1\r\n+key2\r\n:2\r\n");

    const auto result = parser.next_frame();
    ASSERT_TRUE(result.has_value());
    const auto frame = *result;

    EXPECT_EQ(frame.type, FrameType::Map);
    EXPECT_EQ(frame.element_count, 2);  // 2 PAIRS

    FrameIterator iter(frame, parser);

    // Should iterate 4 frames total (Key, Value, Key, Value)
    EXPECT_EQ(iter.remaining(), 4);

    // Key 1
    ASSERT_TRUE(iter.next());
    // Value 1
    ASSERT_TRUE(iter.next());
    // Key 2
    ASSERT_TRUE(iter.next());
    // Value 2
    ASSERT_TRUE(iter.next());

    ASSERT_FALSE(iter.next());
}

TEST_F(ParserTest, ParseNestedArray)
{
    // [ "a", [ "b" ] ]
    feed("*2\r\n+a\r\n*1\r\n+b\r\n");

    const auto result = parser.next_frame();
    ASSERT_TRUE(result.has_value());
    const auto frame = *result;

    FrameIterator iter(frame, parser);

    // First element: "a"
    const auto el1 = iter.next();
    ASSERT_TRUE(el1.has_value());
    EXPECT_EQ(get_simple_payload(*el1), "a");

    // Second element: Nested Array
    const auto el2 = iter.next();
    ASSERT_TRUE(el2.has_value());
    EXPECT_EQ(el2->type, FrameType::Array);

    // Iterate a nested array
    FrameIterator subIter(*el2, parser);
    const auto subEl = subIter.next();
    ASSERT_TRUE(subEl.has_value());
    EXPECT_EQ(get_simple_payload(*subEl), "b");
}


TEST_F(ParserTest, NeedMoreDataPartialCommand)
{
    feed("*2\r\n$3\r\nfoo");

    auto result = parser.next_frame();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::NeedMoreData);

    // Feed rest
    feed("\r\n$3\r\nbar\r\n");
    const auto retry = parser.next_frame();
    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(retry->type, FrameType::Array);
}

TEST_F(ParserTest, MaxRecursiveDepthExceeded)
{
    // Default max depth is 32. Create 100 levels.
    std::string deep;
    for (int i = 0; i < 100; ++i) deep += "*1\r\n";
    deep += ":1\r\n";

    feed(deep);
    auto result = parser.next_frame();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::MaxDepthReached);
}

TEST_F(ParserTest, WideArrayIsOK)
{
    // Make an array with 50 elements (Width > Default Depth 32)
    // This ensures we distinguish between "depth" and "width"
    std::string wide = "*50\r\n";
    for (int i = 0; i < 50; ++i) wide += ":1\r\n";

    feed(wide);
    const auto result = parser.next_frame();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->element_count, 50);
}

TEST_F(ParserTest, MalformedInteger)
{
    feed(":abc\r\n");
    auto result = parser.next_frame();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::Atoi);
}

TEST_F(ParserTest, BulkStringIncomplete)
{
    // $100... but buffer ends early (valid size, just missing data)
    feed("$100\r\nshort\r\n");
    auto result = parser.next_frame();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::NeedMoreData);
}

TEST_F(ParserTest, BulkStringSizeOverflow)
{
    // Length is technically valid int64 but exceeds max allowed buffer/string size
    feed("$9223372036854775807\r\n");
    auto result = parser.next_frame();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::SizeOverflow);
}

TEST_F(ParserTest, ProtocolViolation)
{
    // Array count -1
    feed("*-1\r\n");
    auto result = parser.next_frame();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::MalformedFrame);
}

TEST_F(ParserTest, MixedArrayTypes)
{
    // Array with Integer, SimpleString, Boolean, Double, BulkString
    feed("*5\r\n:123\r\n+hello\r\n#f\r\n,12.34\r\n$5\r\nworld\r\n");

    const auto result = parser.next_frame();
    ASSERT_TRUE(result.has_value());
    const auto frame = *result;

    EXPECT_EQ(frame.type, FrameType::Array);
    EXPECT_EQ(frame.element_count, 5);

    FrameIterator iter(frame, parser);

    // 1. Integer
    const auto i = iter.next();
    ASSERT_TRUE(i.has_value());
    EXPECT_EQ(i->type, FrameType::Integer);
    EXPECT_EQ(get_simple_payload(*i), "123");

    // 2. Simple String
    const auto s = iter.next();
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->type, FrameType::SimpleString);
    EXPECT_EQ(get_simple_payload(*s), "hello");

    // 3. Boolean
    const auto b = iter.next();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->type, FrameType::Boolean);
    EXPECT_EQ(get_simple_payload(*b), "f");

    // 4. Double
    const auto d = iter.next();
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->type, FrameType::Double);
    EXPECT_EQ(get_simple_payload(*d), "12.34");

    // 5. Bulk String
    const auto bs = iter.next();
    ASSERT_TRUE(bs.has_value());
    EXPECT_EQ(bs->type, FrameType::BulkString);
    EXPECT_EQ(get_bulk_payload(*bs), "world");

    ASSERT_FALSE(iter.next());
}

TEST_F(ParserTest, Fuzz)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    for (int iter = 0; iter < 10000; ++iter)  // Reduced count for quick CI
    {
        // Generate random bytes
        std::vector<char> random_data(1024);
        for (auto& b: random_data)
        {
            b = static_cast<char>(dis(gen));
        }

        parser.buffer().extend_from_slice({random_data.data(), random_data.size()});

        // Should not crash, just return error or NeedMoreData
        auto frame = parser.next_frame();

        // Reset for next iteration
        parser.buffer().clear();
    }
}
