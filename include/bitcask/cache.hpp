#pragma once
#include "absl/container/flat_hash_map.h"

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <utility>

namespace bitcask
{

/// @brief Single-threaded Sieve cache with PMR (Polymorphic Memory Resource) support.
/// @tparam Key Must be hashable and movable (absl::flat_hash_map requirements)
/// @tparam Value Must be movable; copying may throw
///
/// Uses std::pmr::memory_resource for all node allocations.
/// Ideal for: low-latency systems, memory-constrained environments, or testing with mock allocators.
///
/// Complexity:
/// - get(): O(1) amortized
/// - put(): O(1) amortized
/// - evict(): O(k) worst-case, O(1) amortized (k = visited nodes scanned)
///
/// Example of usages - Arena fast but no per-node free
/// @code
/// # Arena fast but no per-node free
/// #include <memory_resource>
/// std::pmr::monotonic_buffer_resource arena{1024 * 1024};  // 1MB arena
/// Cache<std::string, int> cache(1000, &arena);
/// // Note: arena memory is freed only when arena is destroyed
/// @endcode
///
/// Example of usages - Pool allocator (reuses fixed-size blocks)
/// @code
/// #include <memory_resource>
/// std::pmr::unsynchronized_pool_resource pool{
///    /*max_bytes_per_block=*/256,  // Tune based on Node size
///    /*memory_block_alignment=*/alignof(SieveCache<std::string, int>::Node)
/// };
/// Cache<std::string, int> cache(1000, &pool);
/// // pool.reclaim() can be called periodically to return unused blocks to system
/// @endcode
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

        // Constructor for placement-new
        Node(Key k, Value v, const bool vis, Node* p, Node* n)
            : key(std::move(k)), value(std::move(v)), visited(vis), prev(p), next(n)
        {
        }

        // Destructor (called explicitly before deallocation)
        ~Node() = default;
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

    // private methods
    Node* allocate_node(Key key, Value value)
    {
        std::pmr::polymorphic_allocator<Node> alloc(mr_);
        // new_object automatically rolls back allocation if the constructor throws
        return alloc.template new_object<Node>(std::move(key), std::move(value), false, nullptr, nullptr);
    }
    void deallocate_node(Node* p)
    {
        if (!p)
        {
            return;
        }
        std::pmr::polymorphic_allocator<Node> alloc(mr_);
        alloc.delete_object(p);  // destructor + deallocate
    }
    void insert_after_head(Node* node) noexcept
    {
        assert(node != nullptr);
        node->next = head_->next;
        node->prev = head_;
        head_->next->prev = node;
        head_->next = node;
    }
    void evict()
    {
        assert(!map_.empty() && "evict() called on empty cache");
        Node* candidate = hand_;

        // Wrap hand if it's on head sentinel (defensive)
        if (candidate == head_)
        {
            candidate = tail_->prev;
        }

        // CLOCK-style scan: reset visited flags until we find eviction candidate
        while (candidate->visited)
        {
            candidate->visited = false;
            candidate = candidate->prev;
            if (candidate == head_)
            {
                candidate = tail_->prev;
            }
        }

        // candidate is now the victim (visited == false)
        // Advance hand for next eviction BEFORE unlinking
        hand_ = candidate->prev;

        // Unlink victim from intrusive doubly-linked list (O(1))
        candidate->prev->next = candidate->next;
        candidate->next->prev = candidate->prev;

        // Remove from hash map (O(1) amortized)
        map_.erase(candidate->key);

        // Explicitly destroy + deallocate via PMR
        deallocate_node(candidate);
    }

public:
    explicit Cache(const std::size_t capacity, std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : capacity_(capacity), mr_(mr), map_(capacity_, typename PmrMap::hasher(), typename PmrMap::key_equal())
    {
    }

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    Cache(Cache&&) = delete;
    Cache& operator=(Cache&&) = delete;

    ~Cache()
    {
        clear();
        deallocate_node(head_);
        deallocate_node(tail_);
    }

    /// @brief Retrieve value by key. Marks item as visited (second chance).
    /// @return std::optional<Value>: contains value if found, nullopt otherwise
    /// @throws May throw if Value copy constructor throws (during return)
    [[nodiscard]] std::optional<Value> get(const Key& key)
    {
        auto it = map_.find(key);
        if (it == map_.end())
        {
            return std::nullopt;
        }
        it->second->visited = true;
        return it->second->value;
    }

    /// @brief Insert or update key-value pair. Evicts if at capacity.
    /// @throws May throw on PMR allocation, hash rehash, or Key/Value move/copy
    void put(Key key, Value value)
    {
        auto it = map_.find(key);
        if (it != map_.end())
        {
            // Update existing
            it->second->value = std::move(value);
            it->second->visited = true;
            return;
        }

        if (map_.size() >= capacity_)
        {
            evict();
        }

        Node* node = allocate_node(std::move(key), std::move(value));
        insert_after_head(node);
        map_.insert({std::move(key), node});
    }

    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] bool empty() const noexcept { return map_.empty(); }
    /// @brief Check if key exists (does not mark as visited)
    [[nodiscard]] bool contains(const Key& key) const { return map_.contains(key); }

    void clear() noexcept
    {
        Node* current = head_->next;
        while (current != tail_)
        {
            Node* next_node = current->next;
            // Destroys node, closes FD, returns memory to pool
            deallocate_node(current);
            current = next_node;
        }

        // Reset pointers
        head_->next = tail_;
        tail_->prev = head_;
        hand_ = head_;

        // Clear map
        map_.clear();
    }
};

// Tracking Alloc, for debug
class TrackingResource : public std::pmr::memory_resource
{
public:
    // Expose metrics
    // Accessors for assertions
    std::size_t allocations() const { return allocations_; }
    std::size_t deallocations() const { return deallocations_; }
    std::size_t peak_bytes() const { return peak_bytes_; }
    std::size_t net_allocations() const { return allocations_ - deallocations_; }
    void reset_counts() { allocations_ = deallocations_ = bytes_allocated_ = bytes_freed_ = peak_bytes_ = 0; }

private:
    void* do_allocate(const std::size_t bytes, const std::size_t alignment) override
    {
        allocations_++;
        bytes_allocated_ += bytes;
        peak_bytes_ = std::max(peak_bytes_, bytes_allocated_ - bytes_freed_);
        return upstream_->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, const std::size_t bytes, const std::size_t alignment) override
    {
        deallocations_++;
        bytes_freed_ += bytes;
        upstream_->deallocate(p, bytes, alignment);
    }

    bool do_is_equal(const memory_resource& other) const noexcept override { return this == &other; }

    memory_resource* upstream_ = std::pmr::get_default_resource();
    std::size_t allocations_ = 0;
    std::size_t deallocations_ = 0;
    std::size_t bytes_allocated_ = 0;
    std::size_t bytes_freed_ = 0;
    std::size_t peak_bytes_ = 0;
};
}  // namespace bitcask