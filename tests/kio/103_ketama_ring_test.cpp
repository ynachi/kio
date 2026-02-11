#include <gtest/gtest.h>
#include <map>
#include <string>

#include "kio/ketama_ring.hpp"

using namespace kio;

TEST(KetamaRingTest, BasicOperations)
{
    KetamaRing<int> ring;

    // Add nodes
    ring.AddNode(0);
    ring.AddNode(1);
    ring.AddNode(2);

    // Test consistency
    auto n1 = ring.GetNodeForKey("key1");
    auto n2 = ring.GetNodeForKey("key1");
    ASSERT_TRUE(n1.has_value());
    ASSERT_TRUE(n2.has_value());
    EXPECT_EQ(*n1, *n2);

    auto n3 = ring.GetNodeForKey("another_key");
    ASSERT_TRUE(n3.has_value());
    EXPECT_GE(*n3, 0);
    EXPECT_LE(*n3, 2);
}

TEST(KetamaRingTest, Replication)
{
    KetamaRing<std::string> ring;
    ring.AddNode("node-A");
    ring.AddNode("node-B");
    ring.AddNode("node-C");
    ring.AddNode("node-D");

    auto nodes = ring.GetNodesForKey("my-key", 3);
    EXPECT_EQ(nodes.size(), 3);

    // Ensure nodes are distinct
    std::unordered_set<std::string> distinct(nodes.begin(), nodes.end());
    EXPECT_EQ(distinct.size(), 3);
}

TEST(KetamaRingTest, Weights)
{
    KetamaRing<int> ring;
    // node 1 has 10x more weight than node 0
    ring.AddNode(0, 10);
    ring.AddNode(1, 100);

    std::map<int, int> counts;
    for (int i = 0; i < 1000; ++i)
    {
        auto node = ring.GetNodeForKey("key-" + std::to_string(i));
        counts[*node]++;
    }

    // Node 1 should have significantly more keys
    EXPECT_GT(counts[1], counts[0]);
}

TEST(KetamaRingTest, Removal)
{
    KetamaRing<int> ring;
    ring.AddNode(1);
    ring.AddNode(2);

    auto n1 = ring.GetNodeForKey("test");
    ring.RemoveNode(1);
    ring.RemoveNode(2);

    auto n2 = ring.GetNodeForKey("test");
    EXPECT_FALSE(n2.has_value());
}

TEST(KetamaRingTest, EmptyRing)
{
    KetamaRing<int> ring;
    EXPECT_FALSE(ring.GetNodeForKey("any").has_value());
    EXPECT_TRUE(ring.GetNodesForKey("any", 1).empty());
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
