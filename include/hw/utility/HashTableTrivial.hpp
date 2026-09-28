#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <bit>

#include <hw/utility/Allocator.hpp>

namespace hw::utility {

enum class InsertStatus : uint8_t {
  Inserted,
  KeyExists,   // key already present; table unchanged
  Full,        // node pool exhausted (PoolExhaustion::ReturnNull); table unchanged
};

//
// HashTableTrivial: A non-thread-safe, fixed-size hash table using separate chaining.
// - Nodes come from an AllocatorTrivial pool pre-allocated for 'initialKeyCount' keys; the bucket
//   array is allocated once at construction. insert / find / erase never touch the heap unless
//   the node pool is exhausted and its policy is PoolExhaustion::Fallback.
// - With PoolExhaustion::ReturnNull, inserting beyond capacity fails with InsertStatus::Full.
// - Erase unlinks and recycles the node immediately: no tombstones, so performance does not
//   degrade under heavy insert/erase churn.
// - Bucket count is a power of two (index by mask); the user hash is post-mixed so keys with
//   regular patterns (e.g. sequential or strided integers with an identity std::hash) spread evenly.
//
template <typename KeyType, typename PayloadType, typename Hash = std::hash<KeyType>, typename KeyEqual = std::equal_to<KeyType>>
class HashTableTrivial {
public:
  // Internal struct for hash table distribution statistics
  struct KeyDistribution {
    size_t bucketCnt = 0;
    size_t keyCnt = 0;
    size_t bucketUsedCnt = 0; // count of buckets with at least one key
    size_t collisionCnt = 0; // count of buckets with 2 or more keys
    size_t collisionTotalCnt = 0; // total count of keys across all buckets with 2 or more keys
    size_t chainLengthMax = 0; // maximum number of keys in a single bucket chain
    double chainLengthAvg = 0.0; // average number of keys in buckets with 2 or more keys
  };

  // Node structure for separate chaining. Allocated by AllocatorTrivial.
  struct Node {
    KeyType key;
    PayloadType payload;
    Node* next; // Pointer to the next node in the chain

    Node(const KeyType& k, const PayloadType& p, Node* n = nullptr)
      : key(k), payload(p), next(n) {}
  };

  // Use AllocatorTrivial to manage Node objects
  using NodeTypeAllocator = AllocatorTrivial<Node>;

  // initialKeyCount: node pool size (keys storable without touching the heap)
  // numBuckets: 0 = derive from initialKeyCount (load factor ~0.7); always rounded up to a power of two
  explicit HashTableTrivial(size_t initialKeyCount, size_t numBuckets = 0,
                            PoolExhaustion onExhausted = PoolExhaustion::Fallback)
    : _num_buckets(bucketCount(initialKeyCount, numBuckets)),
      _mask(_num_buckets - 1),
      _buckets(_num_buckets, nullptr),
      _node_allocator(initialKeyCount, onExhausted)
  {
  }

  ~HashTableTrivial() {
    clear(); // Clean up all allocated nodes
  }

  // Deleted copy/move constructors and assignment operators
  HashTableTrivial(const HashTableTrivial&) = delete;
  HashTableTrivial& operator=(const HashTableTrivial&) = delete;
  HashTableTrivial(HashTableTrivial&&) = delete;
  HashTableTrivial& operator=(HashTableTrivial&&) = delete;

  // Insert a key-payload pair; reports duplicate and pool-exhaustion separately.
  InsertStatus tryInsert(const KeyType& key, const PayloadType& payload) {
    Node*& head = _buckets[bucketOf(key)];
    for (Node* current = head; current != nullptr; current = current->next) {
      if (_key_equal(current->key, key)) {
        return InsertStatus::KeyExists;
      }
    }
    Node* node = _node_allocator.allocate();
    if (!node) [[unlikely]] {
      return InsertStatus::Full;
    }
    _node_allocator.construct(node, key, payload, head);
    head = node; // Add to head of chain
    ++_size;
    return InsertStatus::Inserted;
  }

  // Insert a key-payload pair.
  // Returns true if inserted, false if key already exists (or the pool is exhausted, see tryInsert).
  bool insert(const KeyType& key, const PayloadType& payload) {
    return tryInsert(key, payload) == InsertStatus::Inserted;
  }

  // Returns a pointer to the payload, or nullptr if not found.
  PayloadType* find(const KeyType& key) noexcept {
    Node* node = findNode(key);
    return node ? &node->payload : nullptr;
  }

  const PayloadType* find(const KeyType& key) const noexcept {
    const Node* node = findNode(key);
    return node ? &node->payload : nullptr;
  }

  bool contains(const KeyType& key) const noexcept {
    return findNode(key) != nullptr;
  }

  // Erase a key.
  // Returns true if erased, false if key not found.
  bool erase(const KeyType& key) noexcept {
    Node** link = &_buckets[bucketOf(key)];
    while (Node* current = *link) {
      if (_key_equal(current->key, key)) {
        *link = current->next;
        _node_allocator.release(current); // destroy + return node to the pool
        --_size;
        return true;
      }
      link = &current->next;
    }
    return false; // Key not found
  }

  // Clear all elements from the hash table.
  void clear() noexcept {
    for (size_t i = 0; i < _num_buckets; ++i) {
      Node* current = _buckets[i];
      while (current != nullptr) {
        Node* next = current->next;
        _node_allocator.release(current);
        current = next;
      }
      _buckets[i] = nullptr; // Reset bucket head
    }
    _size = 0;
  }

  // Visits every (key, payload); order depends only on the keys and insertion history
  template <typename Fn>
  void for_each(Fn&& fn) const {
    for (size_t i = 0; i < _num_buckets; ++i) {
      for (const Node* n = _buckets[i]; n != nullptr; n = n->next) {
        fn(n->key, n->payload);
      }
    }
  }

  size_t size()        const noexcept { return _size; }
  bool   empty()       const noexcept { return _size == 0; }
  size_t bucketCount() const noexcept { return _num_buckets; }
  size_t capacity()    const noexcept { return _node_allocator.capacity(); }   // keys storable without heap
  const NodeTypeAllocator& allocator() const noexcept { return _node_allocator; }

  // Populate KeyDistribution structure with current hash table statistics.
  void distribution(KeyDistribution& dist) const noexcept {
    dist.bucketCnt = _num_buckets;
    dist.keyCnt = _size;
    dist.bucketUsedCnt = 0;
    dist.collisionCnt = 0;
    dist.collisionTotalCnt = 0;
    dist.chainLengthMax = 0;

    for (size_t i = 0; i < _num_buckets; ++i) {
      size_t chain_length = 0;
      for (const Node* current = _buckets[i]; current != nullptr; current = current->next) {
        ++chain_length;
      }
      if (chain_length > 0) {
        dist.bucketUsedCnt++;
      }
      dist.chainLengthMax = std::max(dist.chainLengthMax, chain_length);
      if (chain_length >= 2) {
        dist.collisionCnt++;
        dist.collisionTotalCnt += chain_length;
      }
    }
    dist.chainLengthAvg = dist.collisionCnt > 0
                          ? static_cast<double>(dist.collisionTotalCnt) / static_cast<double>(dist.collisionCnt)
                          : 0.0;
  }

private:
  static size_t bucketCount(size_t initialKeyCount, size_t numBuckets) {
    if (initialKeyCount == 0) {
      throw std::invalid_argument("initialKeyCount must be greater than 0.");
    }
    if (numBuckets == 0) {
      // Aim for a load factor of around 0.7 (integer math), then round to next power of 2
      numBuckets = initialKeyCount + (initialKeyCount * 3 + 6) / 7;
    }
    // Ensure minimum number of buckets for small tables
    return std::bit_ceil(std::max<size_t>(numBuckets, 8));
  }

  // murmur3 fmix64 finalizer over the user hash: all output bits depend on all input bits
  static constexpr uint64_t mix(uint64_t h) noexcept {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
  }

  size_t bucketOf(const KeyType& key) const noexcept {
    return static_cast<size_t>(mix(static_cast<uint64_t>(_hash_func(key)))) & _mask;
  }

  Node* findNode(const KeyType& key) const noexcept {
    for (Node* current = _buckets[bucketOf(key)]; current != nullptr; current = current->next) {
      if (_key_equal(current->key, key)) {
        return current;
      }
    }
    return nullptr; // Key not found
  }

  size_t _num_buckets;
  size_t _mask;
  std::vector<Node*> _buckets; // Array of chain heads; sized once at construction, never resized
  NodeTypeAllocator _node_allocator; // Allocator to manage Node objects without runtime heap calls
  [[no_unique_address]] Hash _hash_func; // Hash function for KeyType
  [[no_unique_address]] KeyEqual _key_equal; // Equality predicate for KeyType
  size_t _size = 0; // Current number of elements in the table
};

}
