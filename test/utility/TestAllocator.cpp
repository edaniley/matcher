#include <boost/test/unit_test.hpp>
#include <hw/utility/Allocator.hpp>
#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

using hw::utility::AllocatorTrivial;
using hw::utility::PoolExhaustion;

namespace {

struct Item {
  uint64_t id    = 0;
  uint64_t value = 0;
  Item() = default;
  Item(uint64_t i, uint64_t v) : id(i), value(v) {}
};

// alignof 4, sizeof 8: exercises the ALIGNMENT >= sizeof(void*) rule (posix_memalign requirement)
struct SmallAligned {
  int32_t a = 0;
  int32_t b = 0;
};

struct alignas(64) CacheLine {
  uint64_t data[3] = {};
};

// counts constructions / destructions; optionally throws from the constructor
struct Tracked {
  static inline int alive = 0;
  uint64_t id;
  explicit Tracked(uint64_t i, bool fail = false) : id(i) {
    if (fail) throw std::runtime_error("ctor failure");
    ++alive;
  }
  ~Tracked() { --alive; }
};

template <typename T>
bool aligned(const T * p, size_t alignment) {
  return reinterpret_cast<uintptr_t>(p) % alignment == 0;
}

}

// --- compile-time properties
static_assert(AllocatorTrivial<Item>::BLOCK_SIZE == sizeof(Item));
static_assert(AllocatorTrivial<SmallAligned>::ALIGNMENT == sizeof(void *));
static_assert(AllocatorTrivial<SmallAligned>::BLOCK_SIZE == 8);
static_assert(AllocatorTrivial<CacheLine>::ALIGNMENT == 64);
static_assert(AllocatorTrivial<CacheLine>::BLOCK_SIZE == 64);
static_assert(!std::is_copy_constructible_v<AllocatorTrivial<Item>>);
static_assert(!std::is_move_constructible_v<AllocatorTrivial<Item>>);
// AllocatorTrivial<char> or <uint32_t> would fail: static_assert(sizeof(Type) >= 8)

BOOST_AUTO_TEST_SUITE(AllocatorTests)

// ---------------------------------------------------------------- construction / stats

BOOST_AUTO_TEST_CASE(InitialStats) {
  AllocatorTrivial<Item> pool(16);
  BOOST_CHECK_EQUAL(pool.capacity(), 16u);
  BOOST_CHECK_EQUAL(pool.available(), 16u);
  BOOST_CHECK_EQUAL(pool.inUse(), 0u);
  BOOST_CHECK_EQUAL(pool.peakInUse(), 0u);
  BOOST_CHECK_EQUAL(pool.fallbackCount(), 0u);
  BOOST_CHECK_EQUAL(pool.rejectCount(), 0u);
  BOOST_CHECK(!pool.exhausted());
  BOOST_CHECK(pool.exhaustionPolicy() == PoolExhaustion::Fallback);
}

BOOST_AUTO_TEST_CASE(StatsTrackAllocateAndFree) {
  AllocatorTrivial<Item> pool(4);
  Item * a = pool.allocate();
  Item * b = pool.allocate();
  BOOST_CHECK_EQUAL(pool.inUse(), 2u);
  BOOST_CHECK_EQUAL(pool.available(), 2u);
  pool.free(a);
  BOOST_CHECK_EQUAL(pool.inUse(), 1u);
  BOOST_CHECK_EQUAL(pool.available(), 3u);
  BOOST_CHECK_EQUAL(pool.peakInUse(), 2u);        // high-water mark stays
  pool.free(b);
  BOOST_CHECK_EQUAL(pool.inUse(), 0u);
  BOOST_CHECK_EQUAL(pool.available(), 4u);
}

BOOST_AUTO_TEST_CASE(FreeNullIsNoOp) {
  AllocatorTrivial<Item> pool(2);
  pool.free(nullptr);
  pool.release(nullptr);
  pool.destroy(nullptr);
  BOOST_CHECK_EQUAL(pool.inUse(), 0u);
  BOOST_CHECK_EQUAL(pool.available(), 2u);
}

// ---------------------------------------------------------------- layout and order

BOOST_AUTO_TEST_CASE(FreshBlocksAscendFromIndexZero) {
  AllocatorTrivial<Item> pool(8);
  for (size_t i = 0; i < 8; ++i) {
    Item * p = pool.allocate();
    BOOST_CHECK_EQUAL(p, pool.get(i));
    BOOST_CHECK(pool.inPool(p));
  }
  BOOST_CHECK(pool.exhausted());
}

BOOST_AUTO_TEST_CASE(FreedBlocksReusedLifo) {
  AllocatorTrivial<Item> pool(8);
  Item * a = pool.allocate();
  Item * b = pool.allocate();
  Item * c = pool.allocate();
  pool.free(a);
  pool.free(c);
  BOOST_CHECK_EQUAL(pool.allocate(), c);          // most recently freed first
  BOOST_CHECK_EQUAL(pool.allocate(), a);
  BOOST_CHECK_EQUAL(pool.allocate(), pool.get(3)); // then fresh blocks continue
  (void)b;
}

BOOST_AUTO_TEST_CASE(BlocksDistinctAlignedNonOverlapping) {
  AllocatorTrivial<Item> pool(64);
  std::vector<Item *> ptrs;
  for (int i = 0; i < 64; ++i) ptrs.push_back(pool.allocate());
  std::set<Item *> unique(ptrs.begin(), ptrs.end());
  BOOST_CHECK_EQUAL(unique.size(), 64u);
  std::sort(ptrs.begin(), ptrs.end());
  for (size_t i = 0; i < ptrs.size(); ++i) {
    BOOST_CHECK(aligned(ptrs[i], alignof(Item)));
    if (i > 0) {
      BOOST_CHECK_GE(reinterpret_cast<std::byte *>(ptrs[i]) - reinterpret_cast<std::byte *>(ptrs[i - 1]),
                     static_cast<std::ptrdiff_t>(sizeof(Item)));
    }
  }
}

BOOST_AUTO_TEST_CASE(SmallAlignmentTypeWorks) {
  // previously posix_memalign(…, alignof=4, …) failed with EINVAL -> bad_alloc
  AllocatorTrivial<SmallAligned> pool(4);
  SmallAligned * p = pool.create(SmallAligned{1, 2});
  BOOST_REQUIRE(p);
  BOOST_CHECK_EQUAL(p->b, 2);
  BOOST_CHECK(aligned(p, sizeof(void *)));
  pool.release(p);
}

BOOST_AUTO_TEST_CASE(OverAlignedType) {
  AllocatorTrivial<CacheLine> pool(8, PoolExhaustion::Fallback);
  for (int i = 0; i < 10; ++i) {                  // includes 2 fallback blocks
    CacheLine * p = pool.allocate();
    BOOST_CHECK(aligned(p, 64));
  }
  BOOST_CHECK_EQUAL(pool.fallbackCount(), 2u);
}

BOOST_AUTO_TEST_CASE(GetAndInPool) {
  AllocatorTrivial<Item> pool(4);
  BOOST_CHECK(pool.get(3) != nullptr);
  BOOST_CHECK(pool.get(4) == nullptr);
  const auto & cpool = pool;
  BOOST_CHECK_EQUAL(cpool.get(1), pool.get(1));
  BOOST_CHECK(pool.inPool(pool.get(0)));
  BOOST_CHECK(!pool.inPool(nullptr));
  Item stackItem;
  BOOST_CHECK(!pool.inPool(&stackItem));
  auto misaligned = reinterpret_cast<const Item *>(reinterpret_cast<const std::byte *>(pool.get(0)) + 1);
  BOOST_CHECK(!pool.inPool(misaligned));
}

// ---------------------------------------------------------------- exhaustion: Fallback

BOOST_AUTO_TEST_CASE(FallbackAllocatesFromHeap) {
  AllocatorTrivial<Item> pool(2);                 // default policy: Fallback
  Item * a = pool.allocate();
  Item * b = pool.allocate();
  BOOST_CHECK(pool.exhausted());
  Item * c = pool.allocate();
  BOOST_REQUIRE(c);
  BOOST_CHECK(!pool.inPool(c));
  BOOST_CHECK_EQUAL(pool.fallbackCount(), 1u);
  BOOST_CHECK_EQUAL(pool.rejectCount(), 0u);
  BOOST_CHECK_EQUAL(pool.inUse(), 3u);
  BOOST_CHECK_EQUAL(pool.peakInUse(), 3u);

  pool.free(c);                                   // fallback block joins the free list
  BOOST_CHECK_EQUAL(pool.available(), 1u);
  BOOST_CHECK_EQUAL(pool.allocate(), c);          // and is reused, no new heap allocation
  BOOST_CHECK_EQUAL(pool.fallbackCount(), 1u);
  pool.free(a);
  pool.free(b);
  pool.free(c);
  BOOST_CHECK_EQUAL(pool.inUse(), 0u);
  BOOST_CHECK_EQUAL(pool.available(), 3u);        // capacity 2 + 1 fallback block
}

BOOST_AUTO_TEST_CASE(ZeroCapacityFallback) {
  AllocatorTrivial<Item> pool(0);
  BOOST_CHECK(pool.exhausted());
  Item * p = pool.create(1u, 2u);
  BOOST_REQUIRE(p);
  BOOST_CHECK_EQUAL(p->value, 2u);
  BOOST_CHECK_EQUAL(pool.fallbackCount(), 1u);
  BOOST_CHECK(pool.get(0) == nullptr);
  pool.release(p);
}

// ---------------------------------------------------------------- exhaustion: ReturnNull (reject)

BOOST_AUTO_TEST_CASE(ReturnNullRejectsWhenExhausted) {
  AllocatorTrivial<Item> pool(2, PoolExhaustion::ReturnNull);
  Item * a = pool.allocate();
  Item * b = pool.allocate();
  BOOST_REQUIRE(a && b);
  BOOST_CHECK(pool.allocate() == nullptr);
  BOOST_CHECK(pool.create(1u, 1u) == nullptr);
  BOOST_CHECK_EQUAL(pool.rejectCount(), 2u);
  BOOST_CHECK_EQUAL(pool.fallbackCount(), 0u);
  BOOST_CHECK_EQUAL(pool.inUse(), 2u);            // rejects are not counted as in use

  pool.free(a);                                   // capacity available again
  BOOST_CHECK_EQUAL(pool.allocate(), a);
  BOOST_CHECK_EQUAL(pool.rejectCount(), 2u);
  pool.free(a);
  pool.free(b);
}

BOOST_AUTO_TEST_CASE(ZeroCapacityReturnNull) {
  AllocatorTrivial<Item> pool(0, PoolExhaustion::ReturnNull);
  BOOST_CHECK(pool.allocate() == nullptr);
  BOOST_CHECK_EQUAL(pool.rejectCount(), 1u);
}

BOOST_AUTO_TEST_CASE(PolicyCanBeChangedAtRuntime) {
  AllocatorTrivial<Item> pool(1, PoolExhaustion::ReturnNull);
  Item * a = pool.allocate();
  BOOST_CHECK(pool.allocate() == nullptr);
  pool.setExhaustionPolicy(PoolExhaustion::Fallback);
  BOOST_CHECK(pool.exhaustionPolicy() == PoolExhaustion::Fallback);
  Item * b = pool.allocate();
  BOOST_REQUIRE(b);
  BOOST_CHECK_EQUAL(pool.fallbackCount(), 1u);
  pool.setExhaustionPolicy(PoolExhaustion::ReturnNull);
  BOOST_CHECK(pool.allocate() == nullptr);
  BOOST_CHECK_EQUAL(pool.rejectCount(), 2u);
  pool.free(a);
  pool.free(b);
}

BOOST_AUTO_TEST_CASE(OversizedPoolThrows) {
  BOOST_CHECK_THROW(AllocatorTrivial<Item>(std::numeric_limits<size_t>::max() / 2), std::bad_alloc);
}

// ---------------------------------------------------------------- create / release, construct / destroy

BOOST_AUTO_TEST_CASE(CreateReleaseRunConstructorAndDestructor) {
  Tracked::alive = 0;
  {
    AllocatorTrivial<Tracked> pool(4);
    Tracked * a = pool.create(7u);
    Tracked * b = pool.create(8u);
    BOOST_CHECK_EQUAL(Tracked::alive, 2);
    BOOST_CHECK_EQUAL(a->id, 7u);
    BOOST_CHECK_EQUAL(b->id, 8u);
    pool.release(a);
    BOOST_CHECK_EQUAL(Tracked::alive, 1);
    BOOST_CHECK_EQUAL(pool.inUse(), 1u);
    pool.release(b);
  }
  BOOST_CHECK_EQUAL(Tracked::alive, 0);
}

BOOST_AUTO_TEST_CASE(CreateReturnsBlockWhenConstructorThrows) {
  Tracked::alive = 0;
  AllocatorTrivial<Tracked> pool(2);
  BOOST_CHECK_THROW(pool.create(1u, true), std::runtime_error);
  BOOST_CHECK_EQUAL(pool.inUse(), 0u);
  BOOST_CHECK_EQUAL(pool.available(), 2u);
  Tracked * ok = pool.create(2u);                 // the block went back to the pool
  BOOST_CHECK_EQUAL(ok, reinterpret_cast<Tracked *>(pool.get(0)));
  BOOST_CHECK_EQUAL(Tracked::alive, 1);
  pool.release(ok);
}

BOOST_AUTO_TEST_CASE(SeparateConstructDestroy) {
  AllocatorTrivial<Item> pool(10);
  Item * o1 = pool.allocate();
  pool.construct(o1, 1u, 100u);
  BOOST_REQUIRE(o1 != nullptr);
  BOOST_CHECK_EQUAL(o1->id, 1u);
  pool.destroy(o1);
  pool.free(o1);

  // recycling: next allocation reuses the same block
  Item * o2 = pool.allocate();
  pool.construct(o2, 2u, 101u);
  BOOST_CHECK_EQUAL(o1, o2);
  BOOST_CHECK_EQUAL(o2->id, 2u);
  pool.destroy(o2);
  pool.free(o2);
}

// ---------------------------------------------------------------- randomized

// Random create/release mix beyond capacity; every live object keeps its contents intact
// (a free-list write into a live block would corrupt it) and the stats stay consistent.
BOOST_AUTO_TEST_CASE(RandomizedIntegrity) {
  constexpr size_t CAPACITY = 128;
  AllocatorTrivial<Item> pool(CAPACITY);
  std::vector<Item *> live;
  std::mt19937_64 rng(42);
  uint64_t nextId = 1;

  for (int op = 0; op < 50000; ++op) {
    const bool doCreate = live.empty() || (rng() % 100) < (live.size() < 150 ? 55u : 40u);
    if (doCreate) {
      Item * p = pool.create(nextId, nextId * 31);
      BOOST_REQUIRE(p);
      ++nextId;
      live.push_back(p);
    }
    else {
      const size_t idx = rng() % live.size();
      Item * p = live[idx];
      BOOST_REQUIRE_EQUAL(p->value, p->id * 31);
      pool.release(p);
      live[idx] = live.back();
      live.pop_back();
    }
    BOOST_REQUIRE_EQUAL(pool.inUse(), live.size());
  }
  for (Item * p : live) {
    BOOST_REQUIRE_EQUAL(p->value, p->id * 31);
    pool.release(p);
  }
  BOOST_CHECK_EQUAL(pool.inUse(), 0u);
  BOOST_CHECK_GT(pool.peakInUse(), CAPACITY);                       // went beyond the pool
  BOOST_CHECK_EQUAL(pool.available(), CAPACITY + pool.fallbackCount());
  BOOST_CHECK_EQUAL(pool.fallbackCount(), pool.peakInUse() - CAPACITY);   // heap used only for the excess
}

BOOST_AUTO_TEST_SUITE_END()
