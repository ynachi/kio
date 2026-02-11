#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include "library/cpp/xxhash/xxhash.h"

namespace kio
{
    /**
     * @brief Implements a Ketama-style consistent hashing ring.
     *
     * This class maps keys to nodes in a way that minimizes re-mappings when nodes are added or removed.
     * It's designed to be the core of a routing layer in a distributed or partitioned system.
     *
     * @tparam T The type of the node identifier (e.g., std::string for a partition name or server address).
     */
    template <typename T, typename Hash = std::hash<T>, typename Eq = std::equal_to<T>>
    class KetamaRing
    {
    public:
        /**
         * @brief Adds a node to the hash ring.
         *
         * For each node, a number of "virtual nodes" are added to the ring to ensure a more
         * uniform distribution of keys. The number of virtual nodes is proportional to the weight.
         *
         * @param node The node identifier to add.
         * @param weight A value to influence the node's proportion of the ring. Higher weights
         *               give the node a larger share of keys. A typical value is 100-200.
         */
        void AddNode(T node, int weight = 160)
        {
            if (weight <= 0)
            {
                RemoveNode(node);
                return;
            }

            Eq eq{};
            auto it = std::find_if(nodes_.begin(), nodes_.end(),
                                   [&](const auto& pair) { return eq(pair.first, node); });
            if (it != nodes_.end())
            {
                it->second = weight;
            }
            else
            {
                nodes_.push_back({node, weight});
            }
            BuildRing();
        }

        /**
         * @brief Removes a node from the ring.
         *
         * @param node The node identifier to remove.
         */
        void RemoveNode(const T& node)
        {
            Eq eq{};
            auto it = std::remove_if(nodes_.begin(), nodes_.end(),
                                     [&](const auto& pair) { return eq(pair.first, node); });

            if (it != nodes_.end())
            {
                nodes_.erase(it, nodes_.end());
                BuildRing();
            }
        }

        /**
         * @brief Removes all nodes and clears the ring.
         */
        void Clear()
        {
            nodes_.clear();
            ring_.clear();
        }

        /**
         * @brief Number of unique nodes in the ring.
         */
        [[nodiscard]] size_t NodeCount() const { return nodes_.size(); }

        /**
         * @brief Number of virtual points in the ring.
         */
        [[nodiscard]] size_t PointCount() const { return ring_.size(); }

        /**
         * @brief Whether the ring is empty.
         */
        [[nodiscard]] bool Empty() const { return ring_.empty(); }

        /**
         * @brief Gets the node responsible for a given key.
         *
         * It hashes the key and finds the first node at or after that point on the ring.
         * If the key's hash is past the last node, it "wraps around" to the first node.
         *
         * @param key The key to map to a node.
         * @return The node identifier, or std::nullopt if the ring is empty.
         */
        std::optional<T> GetNodeForKey(std::string_view key) const
        {
            auto nodes = GetNodesForKey(key, 1);
            if (nodes.empty())
            {
                return std::nullopt;
            }
            return nodes.front();
        }

        /**
         * @brief Gets up to N distinct nodes responsible for a given key (replication).
         *
         * Traverses the ring starting from the key's hash and returns the first N
         * distinct physical nodes encountered.
         *
         * @param key The key to map to nodes.
         * @param count Number of distinct nodes to return.
         * @return A list of nodes (may be smaller if the ring has fewer unique nodes).
         */
        std::vector<T> GetNodesForKey(std::string_view key, size_t count) const
        {
            std::vector<T> result;
            if (ring_.empty() || count == 0)
            {
                return result;
            }

            if (count > nodes_.size())
            {
                count = nodes_.size();
            }

            result.reserve(count);
            std::unordered_set<T, Hash, Eq> seen;
            seen.reserve(count * 2);

            const uint64_t key_hash = DoHash(key);
            auto it = std::lower_bound(ring_.begin(), ring_.end(), key_hash,
                                       [](const RingPoint& p, uint64_t h) { return p.point < h; });
            if (it == ring_.end())
            {
                it = ring_.begin();
            }

            const size_t ring_size = ring_.size();
            size_t steps = 0;
            const auto idx = static_cast<size_t>(std::distance(ring_.begin(), it));

            while (result.size() < count && steps < ring_size)
            {
                const auto& node = ring_[(idx + steps) % ring_size].node;
                if (seen.insert(node).second)
                {
                    result.push_back(node);
                }
                ++steps;
            }

            return result;
        }

    private:
        /**
         * @brief Rebuilds the entire ring based on the current list of nodes.
         *
         * This is called whenever a node is added or removed.
         */
        void BuildRing()
        {
            ring_.clear();

            size_t total_points = 0;
            for (const auto& [_, weight] : nodes_)
            {
                if (weight > 0)
                {
                    total_points += static_cast<size_t>(weight);
                }
            }
            ring_.reserve(total_points);

            for (const auto& [node, weight] : nodes_)
            {
                if (weight <= 0)
                {
                    continue;
                }

                // The `weight` parameter directly corresponds to the number of virtual points
                // that this node will receive on the ring. A higher weight means more points
                // and thus a proportionally larger share of keys.
                for (int i = 0; i < weight; ++i)
                {
                    // Create a unique string for each virtual point to be hashed.
                    // This requires that the node type T is stream-insertable (operator<<).
                    std::ostringstream oss;
                    oss << node << "-" << i;
                    ring_.push_back({DoHash(oss.str()), node});
                }
            }

            std::sort(ring_.begin(), ring_.end(),
                      [](const RingPoint& a, const RingPoint& b) { return a.point < b.point; });
        }

        /**
         * @brief Hashes a string view into a 64-bit unsigned integer.
         *
         * @param str The string to hash.
         * @return The 64-bit hash value.
         */
        static uint64_t DoHash(std::string_view str)
        {
            // XXH64 is used for speed and safety against collisions.
            return XXH64(str.data(), str.length(), 0);
        }

        struct RingPoint
        {
            uint64_t point;
            T node;
        };

        // The ring itself: a sorted list of points on the circle.
        std::vector<RingPoint> ring_;

        // The authoritative list of nodes and their weights.
        std::vector<std::pair<T, int>> nodes_;
    };
} // namespace kio
