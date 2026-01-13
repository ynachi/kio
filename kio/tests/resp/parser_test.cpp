//
// Created by Yao ACHI on 14/12/2025.
//

#include "kio/resp/parser.h"

#include "kio/core/async_logger.h"

#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace kio::resp;

class ParserTest : public ::testing::Test
{
protected:
    // Initialize with the default config
    Parser parser_{ParserConfig{}};

    void Feed(std::string_view data) { parser_.Buffer().ExtendFromSlice({data.data(), data.size()}); }

    void SetUp() override
    {
        kio::alog::Configure(1024, kio::LogLevel::kDisabled);
        // Ensure a clean state
        parser_.Buffer().Clear();
    }
};

TEST_F(ParserTest, ParseSimpleString)
{
    Feed("+OK\r\n");
    const auto kResult = parser_.NextFrame();
    ASSERT_TRUE(kResult.has_value());

    const auto kFrame = *kResult;
    EXPECT_EQ(kFrame.type, FrameType::kSimpleString);
    EXPECT_EQ(kFrame.size, 5);
    EXPECT_EQ(GetSimplePayload(kFrame), "OK");

    parser_.Consume(kFrame);
    EXPECT_EQ(parser_.Buffer().Consumed(), 5);
}

TEST_F(ParserTest, ParseInteger)
{
    Feed(":1000\r\n:-42\r\n");

    // First integer
    const auto kRes1 = parser_.NextFrame();
    ASSERT_TRUE(kRes1.has_value());
    EXPECT_EQ(kRes1->type, FrameType::kInteger);
    EXPECT_EQ(GetSimplePayload(*kRes1), "1000");
    parser_.Consume(*kRes1);

    // Second integer
    const auto kRes2 = parser_.NextFrame();
    ASSERT_TRUE(kRes2.has_value());
    EXPECT_EQ(kRes2->type, FrameType::kInteger);
    EXPECT_EQ(GetSimplePayload(*kRes2), "-42");
    parser_.Consume(*kRes2);
}

TEST_F(ParserTest, ParseDouble)
{
    Feed(",1000\r\n,-42\r\n,34.5\r\n");

    // First integer
    const auto kRes1 = parser_.NextFrame();
    ASSERT_TRUE(kRes1.has_value());
    EXPECT_EQ(kRes1->type, FrameType::kDouble);
    EXPECT_EQ(GetSimplePayload(*kRes1), "1000");
    parser_.Consume(*kRes1);

    // Second integer
    const auto kRes2 = parser_.NextFrame();
    ASSERT_TRUE(kRes2.has_value());
    EXPECT_EQ(kRes2->type, FrameType::kDouble);
    EXPECT_EQ(GetSimplePayload(*kRes2), "-42");
    parser_.Consume(*kRes2);

    // Fird integer
    const auto res3 = parser_.NextFrame();
    ASSERT_TRUE(res3.has_value());
    EXPECT_EQ(res3->type, FrameType::kDouble);
    EXPECT_EQ(GetSimplePayload(*res3), "34.5");
    parser_.Consume(*res3);
}

TEST_F(ParserTest, ParseBulkString)
{
    Feed("$5\r\nhello\r\n");
    const auto kResult = parser_.NextFrame();
    ASSERT_TRUE(kResult.has_value());

    const auto kFrame = *kResult;
    EXPECT_EQ(kFrame.type, FrameType::kBulkString);
    EXPECT_EQ(GetBulkPayload(kFrame), "hello");
}

TEST_F(ParserTest, ParseEmptyBulkString)
{
    Feed("$0\r\n\r\n");
    const auto kResult = parser_.NextFrame();
    ASSERT_TRUE(kResult.has_value());

    const auto kFrame = *kResult;
    EXPECT_EQ(kFrame.type, FrameType::kBulkString);
    EXPECT_EQ(GetBulkPayload(kFrame), "");
    // Size check: $0\r\n (4) + \r\n (2) = 6
    EXPECT_EQ(kFrame.size, 6);
}

TEST_F(ParserTest, ParseNullBulkString)
{
    Feed("$-1\r\n");
    const auto kResult = parser_.NextFrame();
    ASSERT_TRUE(kResult.has_value());

    const auto kFrame = *kResult;
    EXPECT_EQ(kFrame.type, FrameType::kBulkString);
    EXPECT_TRUE(GetBulkPayload(kFrame).empty());
    EXPECT_EQ(kFrame.size, 5);
}

TEST_F(ParserTest, RejectsBadBulkTerminator)
{
    Feed("$3\r\nabcZZ");

    auto res = parser_.NextFrame();
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ParseError::kMalformedFrame);
}

TEST(RespParserTest, AggregateSizeOverflow)
{
    ParserConfig config;
    config.max_size = 10;
    Parser parser(config);

    const std::string data = "*1\r\n$1\r\na\r\n";
    parser.Buffer().ExtendFromSlice(std::span(data.data(), data.size()));

    auto res = parser.NextFrame();
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ParseError::kSizeOverflow);
}

TEST_F(ParserTest, ParseBoolean)
{
    Feed("#t\r\n#f\r\n");

    const auto kT = parser_.NextFrame();
    ASSERT_TRUE(kT.has_value());
    EXPECT_EQ(GetSimplePayload(*kT), "t");
    parser_.Consume(*kT);

    const auto kF = parser_.NextFrame();
    ASSERT_TRUE(kF.has_value());
    EXPECT_EQ(GetSimplePayload(*kF), "f");
}

TEST_F(ParserTest, ParseArray)
{
    // Array of 3 items: [1, 2, 3]
    Feed("*3\r\n:1\r\n:2\r\n:3\r\n");

    auto result = parser_.NextFrame();
    ASSERT_TRUE(result.has_value());
    auto frame = *result;

    EXPECT_EQ(frame.type, FrameType::kArray);
    EXPECT_EQ(frame.element_count, 3);

    // Verify Iterator
    FrameIterator iter(frame, parser_);

    auto child1 = iter.Next();
    ASSERT_TRUE(child1);
    EXPECT_EQ(GetSimplePayload(*child1), "1");

    auto child2 = iter.Next();
    ASSERT_TRUE(child2);
    EXPECT_EQ(GetSimplePayload(*child2), "2");

    auto child3 = iter.Next();
    ASSERT_TRUE(child3);
    EXPECT_EQ(GetSimplePayload(*child3), "3");

    ASSERT_FALSE(iter.Next());
}

TEST_F(ParserTest, ParseMap)
{
    // Map with 2 pairs
    Feed("%2\r\n+key1\r\n:1\r\n+key2\r\n:2\r\n");

    const auto kResult = parser_.NextFrame();
    ASSERT_TRUE(kResult.has_value());
    const auto kFrame = *kResult;

    EXPECT_EQ(kFrame.type, FrameType::kMap);
    EXPECT_EQ(kFrame.element_count, 2);  // 2 PAIRS

    FrameIterator iter(kFrame, parser_);

    // Should iterate 4 frames total (Key, Value, Key, Value)
    EXPECT_EQ(iter.Remaining(), 4);

    // Key 1
    ASSERT_TRUE(iter.Next());
    // Value 1
    ASSERT_TRUE(iter.Next());
    // Key 2
    ASSERT_TRUE(iter.Next());
    // Value 2
    ASSERT_TRUE(iter.Next());

    ASSERT_FALSE(iter.Next());
}

TEST_F(ParserTest, ParseNestedArray)
{
    // [ "a", [ "b" ] ]
    Feed("*2\r\n+a\r\n*1\r\n+b\r\n");

    const auto kResult = parser_.NextFrame();
    ASSERT_TRUE(kResult.has_value());
    const auto kFrame = *kResult;

    FrameIterator iter(kFrame, parser_);

    // First element: "a"
    const auto kEl1 = iter.Next();
    ASSERT_TRUE(kEl1.has_value());
    EXPECT_EQ(GetSimplePayload(*kEl1), "a");

    // Second element: Nested Array
    const auto el2 = iter.Next();
    ASSERT_TRUE(el2.has_value());
    EXPECT_EQ(el2->type, FrameType::kArray);

    // Iterate a nested array
    FrameIterator sub_iter(*el2, parser_);
    const auto kSubEl = sub_iter.Next();
    ASSERT_TRUE(kSubEl.has_value());
    EXPECT_EQ(GetSimplePayload(*kSubEl), "b");
}

TEST_F(ParserTest, NeedMoreDataPartialCommand)
{
    Feed("*2\r\n$3\r\nfoo");

    auto result = parser_.NextFrame();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kNeedMoreData);

    // Feed rest
    Feed("\r\n$3\r\nbar\r\n");
    const auto kRetry = parser_.NextFrame();
    ASSERT_TRUE(kRetry.has_value());
    EXPECT_EQ(kRetry->type, FrameType::kArray);
}

TEST_F(ParserTest, MaxRecursiveDepthExceeded)
{
    // Default max depth is 32. Create 100 levels.
    std::string deep;
    for (int i = 0; i < 100; ++i)
    {
        deep += "*1\r\n";
    }
    deep += ":1\r\n";

    Feed(deep);
    auto result = parser_.NextFrame();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kMaxDepthReached);
}

TEST_F(ParserTest, WideArrayIsOK)
{
    // Make an array with 50 elements (Width > Default Depth 32)
    // This ensures we distinguish between "depth" and "width"
    std::string wide = "*50\r\n";
    for (int i = 0; i < 50; ++i)
    {
        wide += ":1\r\n";
    }

    Feed(wide);
    const auto kResult = parser_.NextFrame();
    ASSERT_TRUE(kResult.has_value());
    EXPECT_EQ(kResult->element_count, 50);
}

TEST_F(ParserTest, MalformedInteger)
{
    Feed(":abc\r\n");
    auto result = parser_.NextFrame();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kAtoi);
}

TEST_F(ParserTest, BulkStringIncomplete)
{
    // $100... but buffer ends early (valid size, just missing data)
    Feed("$100\r\nshort\r\n");
    auto result = parser_.NextFrame();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kNeedMoreData);
}

TEST_F(ParserTest, BulkStringSizeOverflow)
{
    // Length is technically valid int64 but exceeds max allowed buffer/string size
    Feed("$9223372036854775807\r\n");
    auto result = parser_.NextFrame();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kSizeOverflow);
}

TEST_F(ParserTest, NullArray)
{
    // Array count -1
    Feed("*-1\r\n");
    auto result = parser_.NextFrame();
    ASSERT_TRUE(result.has_value());
}

TEST_F(ParserTest, MixedArrayTypes)
{
    // Array with Integer, SimpleString, Boolean, Double, BulkString
    Feed("*5\r\n:123\r\n+hello\r\n#f\r\n,12.34\r\n$5\r\nworld\r\n");

    const auto kResult = parser_.NextFrame();
    ASSERT_TRUE(kResult.has_value());
    const auto kFrame = *kResult;

    EXPECT_EQ(kFrame.type, FrameType::kArray);
    EXPECT_EQ(kFrame.element_count, 5);

    FrameIterator iter(kFrame, parser_);

    // 1. Integer
    const auto i = iter.Next();
    ASSERT_TRUE(i.has_value());
    EXPECT_EQ(i->type, FrameType::kInteger);
    EXPECT_EQ(GetSimplePayload(*i), "123");

    // 2. Simple String
    const auto s = iter.Next();
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->type, FrameType::kSimpleString);
    EXPECT_EQ(GetSimplePayload(*s), "hello");

    // 3. Boolean
    const auto b = iter.Next();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->type, FrameType::kBoolean);
    EXPECT_EQ(GetSimplePayload(*b), "f");

    // 4. Double
    const auto d = iter.Next();
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->type, FrameType::kDouble);
    EXPECT_EQ(GetSimplePayload(*d), "12.34");

    // 5. Bulk String
    const auto bs = iter.Next();
    ASSERT_TRUE(bs.has_value());
    EXPECT_EQ(bs->type, FrameType::kBulkString);
    EXPECT_EQ(GetBulkPayload(*bs), "world");

    ASSERT_FALSE(iter.Next());
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
        for (auto& b : random_data)
        {
            b = static_cast<char>(dis(gen));
        }

        parser_.Buffer().ExtendFromSlice({random_data.data(), random_data.size()});

        // Should not crash, just return error or NeedMoreData
        auto frame = parser_.NextFrame();

        // Reset for next iteration
        parser_.Buffer().Clear();
    }
}
