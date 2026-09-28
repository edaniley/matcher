#pragma once

#include <cstdint>
#include <cstdlib>   // For posix_memalign, free
#include <new>       // For placement new, std::bad_alloc
#include <limits>
#include <vector>
#include <utility>   // For std::forward
#include <algorithm> // For std::max

namespace hw::utility {

// What AllocatorTrivial does when the pre-allocated pool is exhausted
enum class PoolExhaustion : uint8_t {
  Fallback,     // allocate the block from the heap (jitter path; counted in fallbackCount())
  ReturnNull,   // reject: allocate() / create() return nullptr (counted in rejectCount())
};

/**
 * AllocatorTrivial: A high-performance pool allocator for a specific Type.
 * - Pre-allocates 'count' blocks at construction; allocate() / free() are O(1) with no heap access.
 * - Free blocks form an intrusive LIFO free list stored inside the idle blocks themselves,
 *   so Type must be at least 8 bytes (one pointer).
 * - Every page of the pool is touched at construction (free-list links are written),
 *   so there are no page faults on the hot path.
 * - Fresh blocks are handed out in ascending address order; freed blocks are reused LIFO
 *   (most recently freed first, which is the most likely to still be in cache).
 * - Behaviour on exhaustion is configurable (PoolExhaustion).
 * - Raw allocate()/free() plus construct()/destroy(), or the combined create()/release().
 * - Not thread safe. The allocator does not track live objects: the caller must destroy
 *   every object it created before the allocator itself is destroyed.
 */
template <typename Type>
class AllocatorTrivial {
  // A node in our free-list, stored directly in the idle memory blocks
  struct Node {
    Node * next;
  };

  static_assert(sizeof(Node) <= 8);
  static_assert(sizeof(Type) >= 8, "AllocatorTrivial: Type must be at least 8 bytes (idle blocks hold a free-list pointer)");

public:
  // posix_memalign requires a power of two that is a multiple of sizeof(void *)
  static constexpr size_t ALIGNMENT  = std::max({alignof(Type), alignof(Node), sizeof(void *)});
  static constexpr size_t BLOCK_SIZE = (sizeof(Type) + ALIGNMENT - 1) & ~(ALIGNMENT - 1);

  static_assert((ALIGNMENT & (ALIGNMENT - 1)) == 0, "alignment must be a power of two");

  explicit AllocatorTrivial(size_t count, PoolExhaustion onExhausted = PoolExhaustion::Fallback)
    : _count(count), _policy(onExhausted)
  {
    if (_count == 0) return;   // zero-sized pool: every allocation takes the exhaustion path
    if (_count > std::numeric_limits<size_t>::max() / BLOCK_SIZE) {
      throw std::bad_alloc();
    }
    if (posix_memalign(&_data, ALIGNMENT, _count * BLOCK_SIZE) != 0) {
      throw std::bad_alloc();
    }
    // push in reverse, so the first allocation returns block 0 and fresh blocks ascend
    std::byte * base = static_cast<std::byte *>(_data);
    for (size_t i = _count; i > 0; --i) {
      push(base + (i - 1) * BLOCK_SIZE);
    }
    _available = _count;
  }

  ~AllocatorTrivial() {
    // Free any memory obtained via the fallback path
    for (void * ptr : _postalloc) {
      ::free(ptr);
    }
    // Free the main pre-allocated pool
    ::free(_data);
  }

  AllocatorTrivial(const AllocatorTrivial &) = delete;
  AllocatorTrivial & operator = (const AllocatorTrivial &) = delete;
  AllocatorTrivial(AllocatorTrivial &&) = delete;
  AllocatorTrivial & operator = (AllocatorTrivial &&) = delete;

  // Allocate raw memory for one object of Type. Returns uninitialized memory, or nullptr
  // if the pool is exhausted and the policy is ReturnNull.
  inline Type * allocate() {
    if (_free) [[likely]] {
      Node * node = _free;
      _free = node->next;
      --_available;
      noteAllocated();
      return reinterpret_cast<Type *>(node);
    }
    return allocateExhausted();
  }

  // Deallocate raw memory. The destructor for Type is NOT called; see release().
  inline void free(Type * ptr) noexcept {
    if (!ptr) return;
    push(reinterpret_cast<std::byte *>(ptr));
    ++_available;
    --_inUse;
  }

  // Construct an object of Type at the given pre-allocated memory location.
  template <typename ... Args>
  inline void construct(Type * ptr, Args &&... args) {
    new (ptr) Type(std::forward<Args>(args)...);
  }

  // Destroy an object of Type at the given memory location.
  inline void destroy(Type * ptr) noexcept {
    if (ptr) {
      ptr->~Type();
    }
  }

  // allocate() + construct(). Returns nullptr if rejected (ReturnNull policy).
  // If the constructor throws, the block is returned to the pool and the exception propagates.
  template <typename ... Args>
  Type * create(Args &&... args) {
    Type * ptr = allocate();
    if (!ptr) [[unlikely]] return nullptr;
    try {
      construct(ptr, std::forward<Args>(args)...);
    }
    catch (...) {
      free(ptr);
      throw;
    }
    return ptr;
  }

  // destroy() + free()
  void release(Type * ptr) noexcept {
    if (!ptr) return;
    destroy(ptr);
    free(ptr);
  }

  // Get a pointer to the block at a specific index within the main pre-allocated pool.
  // Useful for direct indexed access; the caller must know the block is in use.
  Type * get(size_t idx) noexcept {
    if (idx < _count) {
      return reinterpret_cast<Type *>(static_cast<std::byte *>(_data) + idx * BLOCK_SIZE);
    }
    return nullptr; // Index out of bounds of the initial pool
  }

  const Type * get(size_t idx) const noexcept {
    if (idx < _count) {
      return reinterpret_cast<const Type *>(static_cast<const std::byte *>(_data) + idx * BLOCK_SIZE);
    }
    return nullptr;
  }

  // true if ptr is a block of the main pre-allocated pool (not a fallback block)
  bool inPool(const Type * ptr) const noexcept {
    const auto p     = reinterpret_cast<uintptr_t>(ptr);
    const auto begin = reinterpret_cast<uintptr_t>(_data);
    return _data && p >= begin && p < begin + _count * BLOCK_SIZE && (p - begin) % BLOCK_SIZE == 0;
  }

  // --- configuration ---
  PoolExhaustion exhaustionPolicy() const noexcept { return _policy; }
  void setExhaustionPolicy(PoolExhaustion policy) noexcept { _policy = policy; }

  // --- usage statistics ---
  size_t capacity()      const noexcept { return _count; }          // blocks pre-allocated at construction
  size_t available()     const noexcept { return _available; }      // blocks on the free list (may include freed fallback blocks)
  size_t inUse()         const noexcept { return _inUse; }          // blocks currently allocated
  size_t peakInUse()     const noexcept { return _peakInUse; }      // high-water mark of inUse()
  size_t fallbackCount() const noexcept { return _postalloc.size(); } // heap allocations made on exhaustion
  size_t rejectCount()   const noexcept { return _rejectCount; }    // allocations refused on exhaustion
  bool   exhausted()     const noexcept { return _free == nullptr; }

private:
  // cold path: free list is empty
  [[gnu::noinline]] Type * allocateExhausted() {
    if (_policy == PoolExhaustion::ReturnNull) {
      ++_rejectCount;
      return nullptr;
    }
    // Fallback: This is the "jitter" path - indicates the initial pool is too small.
    void * ptr = nullptr;
    if (posix_memalign(&ptr, ALIGNMENT, BLOCK_SIZE) != 0) {
      throw std::bad_alloc();
    }
    try {
      _postalloc.push_back(ptr);   // Keep track of fallback allocations for freeing
    }
    catch (...) {
      ::free(ptr);
      throw;
    }
    noteAllocated();
    return static_cast<Type *>(ptr);
  }

  inline void noteAllocated() noexcept {
    if (++_inUse > _peakInUse) _peakInUse = _inUse;
  }

  // Push a raw memory block to the free list.
  inline void push(std::byte * ptr) noexcept {
    _free = new (ptr) Node{_free};
  }

  void *         _data        = nullptr;  // start of the main pre-allocated memory pool
  Node *         _free        = nullptr;  // head of the free list
  size_t         _count       = 0;        // number of blocks in the initial pool
  PoolExhaustion _policy      = PoolExhaustion::Fallback;
  size_t         _available   = 0;
  size_t         _inUse       = 0;
  size_t         _peakInUse   = 0;
  size_t         _rejectCount = 0;

  // Stores pointers to memory blocks allocated during fallback (when free list is empty)
  std::vector<void *> _postalloc;
};

} // namespace hw::utility
