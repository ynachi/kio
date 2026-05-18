#pragma once
// #include "absl/container/flat_hash_map.h"

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>

namespace bitcask
{

template <typename Key, typename Value>
class Cache
{
    struct Node
    {
        Key key;
        Value value;
        bool visited;
        Node* prev;
        Node* next;
    };

    // Alias for a PMR-aware Abseil hash map
    using PmrMap = absl::flat_hash_map<Key, Node*, typename absl::flat_hash_map<Key, Node*>::hasher,
                                       typename absl::flat_hash_map<Key, Node*>::key_equal,
                                       std::pmr::polymorphic_allocator<std::pair<const Key, Node*>>>;

    // members
    std::size_t capacity_;
    std::pmr::memory_resource* mr_;
    Node* head_ = nullptr;
    Node* tail_ = nullptr;
    Node* hand_ = nullptr;
    PmrMap map_;

public:
    explicit Cache(std::size_t capacity, std::pmr::memory_resource* mr = std::pmr::get_default_resource());
};
}  // namespace bitcask