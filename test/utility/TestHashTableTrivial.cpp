#include <boost/test/unit_test.hpp>
#include <hw/utility/HashTableTrivial.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>

using hw::utility::HashTableTrivial;
using hw::utility::InsertStatus;
using hw::utility::PoolExhaustion;

namespace {

using Table = HashTableTrivial<uint64_t, int>;

// every key hashes to the same value: forces all keys into one chain
struct ConstantHash {
  size_t operator()(uint64_t) const noexcept { return 42; }
};
using CollidingTable = HashTableTrivial<uint64_t, int, ConstantHash>;

// composite key with custom hash / equality
struct Key3 {
  uint32_t a, b, c;
};
struct Key3Hash {
  size_t operator()(const Key3 & k) const noexcept {
    return (static_cast<size_t>(k.a) << 40) ^ (static_cast<size_t>(k.b) << 20) ^ k.c;
  }
};
struct Key3Equal {
  bool operator()(const Key3 & x, const Key3 & y) const noexcept {
    return x.a == y.a && x.b == y.b && x.c == y.c;
  }
};

struct Payload {
  uint64_t id;
  void *   ptr;
};

}

BOOST_AUTO_TEST_SUITE(HashTableTrivialTests)

// ---------------------------------------------------------------- construction

BOOST_AUTO_TEST_CASE(ZeroCapacityThrows) {
  BOOST_CHECK_THROW(Table(0), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(BucketCountIsPowerOfTwo) {
  BOOST_CHECK_EQUAL(Table(1).bucketCount(), 8u);           // minimum
  BOOST_CHECK_EQUAL(Table(7).bucketCount(), 16u);          // 7 / 0.7 = 10 -> 16
  BOOST_CHECK_EQUAL(Table(1000).bucketCount(), 2048u);     // ~1429 -> 2048
  BOOST_CHECK_EQUAL(Table(10, 100).bucketCount(), 128u);   // explicit count rounded up
  BOOST_CHECK_EQUAL(Table(10, 64).bucketCount(), 64u);
  for (size_t n : {1u, 3u, 100u, 12345u}) {
    const size_t b = Table(n).bucketCount();
    BOOST_CHECK_EQUAL(b & (b - 1), 0u);
    BOOST_CHECK_GE(b * 7, n * 10);                         // load factor <= ~0.7
  }
}

BOOST_AUTO_TEST_CASE(InitialState) {
  Table t(16);
  BOOST_CHECK(t.empty());
  BOOST_CHECK_EQUAL(t.size(), 0u);
  BOOST_CHECK_EQUAL(t.capacity(), 16u);
  BOOST_CHECK(t.find(1) == nullptr);
  BOOST_CHECK(!t.contains(1));
  BOOST_CHECK(!t.erase(1));
}

// ---------------------------------------------------------------- insert / find / erase

BOOST_AUTO_TEST_CASE(InsertFindErase) {
  Table t(16);
  BOOST_CHECK(t.tryInsert(10, 100) == InsertStatus::Inserted);
  BOOST_CHECK(t.insert(20, 200));
  BOOST_CHECK_EQUAL(t.size(), 2u);
  BOOST_REQUIRE(t.find(10));
  BOOST_CHECK_EQUAL(*t.find(10), 100);
  BOOST_CHECK_EQUAL(*t.find(20), 200);
  BOOST_CHECK(t.contains(20));
  BOOST_CHECK(t.erase(10));
  BOOST_CHECK(t.find(10) == nullptr);
  BOOST_CHECK(!t.erase(10));
  BOOST_CHECK_EQUAL(t.size(), 1u);
  BOOST_CHECK_EQUAL(*t.find(20), 200);
}

BOOST_AUTO_TEST_CASE(DuplicateKeyRejectedValueUnchanged) {
  Table t(4);
  t.insert(5, 50);
  BOOST_CHECK(t.tryInsert(5, 51) == InsertStatus::KeyExists);
  BOOST_CHECK(!t.insert(5, 52));
  BOOST_CHECK_EQUAL(*t.find(5), 50);
  BOOST_CHECK_EQUAL(t.size(), 1u);
  BOOST_CHECK_EQUAL(t.allocator().inUse(), 1u);           // no node consumed by the duplicate
}

BOOST_AUTO_TEST_CASE(PayloadMutableThroughFind) {
  Table t(4);
  t.insert(1, 10);
  *t.find(1) = 11;
  BOOST_CHECK_EQUAL(*t.find(1), 11);
  const Table & c = t;
  BOOST_CHECK_EQUAL(*c.find(1), 11);
}

BOOST_AUTO_TEST_CASE(ReinsertAfterErase) {
  Table t(4);
  t.insert(7, 1);
  t.erase(7);
  BOOST_CHECK(t.insert(7, 2));
  BOOST_CHECK_EQUAL(*t.find(7), 2);
}

BOOST_AUTO_TEST_CASE(ZeroKeyAndExtremeKeys) {
  Table t(8);
  for (uint64_t k : {uint64_t(0), uint64_t(1), UINT64_MAX, UINT64_MAX - 1, uint64_t(1) << 63}) {
    BOOST_CHECK(t.insert(k, static_cast<int>(k & 0xFF)));
  }
  BOOST_CHECK_EQUAL(t.size(), 5u);
  BOOST_CHECK(t.contains(0));
  BOOST_CHECK(t.contains(UINT64_MAX));
  BOOST_CHECK(t.erase(0));
  BOOST_CHECK(!t.contains(0));
}

// ---------------------------------------------------------------- collisions (single chain)

BOOST_AUTO_TEST_CASE(EraseHeadMiddleTailOfChain) {
  CollidingTable t(8);
  for (uint64_t k = 1; k <= 5; ++k) t.insert(k, static_cast<int>(k * 10));
  HashTableTrivial<uint64_t, int, ConstantHash>::KeyDistribution d;
  t.distribution(d);
  BOOST_CHECK_EQUAL(d.bucketUsedCnt, 1u);
  BOOST_CHECK_EQUAL(d.chainLengthMax, 5u);

  BOOST_CHECK(t.erase(5));                                // head (inserted last)
  BOOST_CHECK(t.erase(3));                                // middle
  BOOST_CHECK(t.erase(1));                                // tail
  BOOST_CHECK(!t.erase(3));
  BOOST_CHECK_EQUAL(t.size(), 2u);
  BOOST_CHECK_EQUAL(*t.find(2), 20);
  BOOST_CHECK_EQUAL(*t.find(4), 40);
  BOOST_CHECK(t.find(1) == nullptr && t.find(3) == nullptr && t.find(5) == nullptr);
}

BOOST_AUTO_TEST_CASE(DuplicateDetectedDeepInChain) {
  CollidingTable t(8);
  for (uint64_t k = 1; k <= 6; ++k) t.insert(k, 0);
  BOOST_CHECK(t.tryInsert(1, 9) == InsertStatus::KeyExists);   // first inserted = end of chain
  BOOST_CHECK_EQUAL(t.size(), 6u);
}

// ---------------------------------------------------------------- distribution / hashing

BOOST_AUTO_TEST_CASE(StridedKeysSpreadEvenly) {
  // std::hash<uint64_t> is the identity in libstdc++; without post-mixing, keys that are
  // multiples of the bucket count would all land in bucket 0
  constexpr size_t N = 4096;
  Table t(N);
  for (uint64_t i = 0; i < N; ++i) BOOST_REQUIRE(t.insert(i * 8192, 0));
  Table::KeyDistribution d;
  t.distribution(d);
  BOOST_CHECK_EQUAL(d.keyCnt, N);
  BOOST_CHECK_GT(d.bucketUsedCnt, N / 2);                  // most keys have their own bucket
  BOOST_CHECK_LE(d.chainLengthMax, 10u);
}

BOOST_AUTO_TEST_CASE(DistributionStats) {
  CollidingTable t(8);
  t.insert(1, 0);
  t.insert(2, 0);
  t.insert(3, 0);
  CollidingTable::KeyDistribution d;
  t.distribution(d);
  BOOST_CHECK_EQUAL(d.bucketCnt, t.bucketCount());
  BOOST_CHECK_EQUAL(d.keyCnt, 3u);
  BOOST_CHECK_EQUAL(d.bucketUsedCnt, 1u);
  BOOST_CHECK_EQUAL(d.collisionCnt, 1u);
  BOOST_CHECK_EQUAL(d.collisionTotalCnt, 3u);
  BOOST_CHECK_EQUAL(d.chainLengthMax, 3u);
  BOOST_CHECK_CLOSE(d.chainLengthAvg, 3.0, 1e-9);

  Table empty(8);
  Table::KeyDistribution e;
  empty.distribution(e);
  BOOST_CHECK_EQUAL(e.bucketUsedCnt, 0u);
  BOOST_CHECK_EQUAL(e.chainLengthAvg, 0.0);
}

// ---------------------------------------------------------------- capacity / exhaustion

BOOST_AUTO_TEST_CASE(ReturnNullReportsFullAndRecovers) {
  Table t(3, 0, PoolExhaustion::ReturnNull);
  BOOST_CHECK(t.insert(1, 1));
  BOOST_CHECK(t.insert(2, 2));
  BOOST_CHECK(t.insert(3, 3));
  BOOST_CHECK(t.tryInsert(4, 4) == InsertStatus::Full);
  BOOST_CHECK(!t.insert(4, 4));
  BOOST_CHECK(t.tryInsert(1, 9) == InsertStatus::KeyExists);   // duplicate still reported as such
  BOOST_CHECK_EQUAL(t.size(), 3u);
  BOOST_CHECK(!t.contains(4));
  BOOST_CHECK_EQUAL(t.allocator().fallbackCount(), 0u);

  t.erase(2);                                              // node recycled
  BOOST_CHECK(t.tryInsert(4, 4) == InsertStatus::Inserted);
  BOOST_CHECK_EQUAL(*t.find(4), 4);
}

BOOST_AUTO_TEST_CASE(FallbackGrowsBeyondCapacity) {
  Table t(2);                                              // default: Fallback
  for (uint64_t k = 0; k < 10; ++k) BOOST_CHECK(t.insert(k, static_cast<int>(k)));
  BOOST_CHECK_EQUAL(t.size(), 10u);
  BOOST_CHECK_EQUAL(t.allocator().fallbackCount(), 8u);
  for (uint64_t k = 0; k < 10; ++k) BOOST_CHECK_EQUAL(*t.find(k), static_cast<int>(k));
}

BOOST_AUTO_TEST_CASE(ChurnDoesNotConsumeCapacity) {
  // repeated insert/erase of distinct keys: nodes are recycled, never leaked
  Table t(4, 0, PoolExhaustion::ReturnNull);
  for (uint64_t k = 0; k < 100000; ++k) {
    BOOST_REQUIRE(t.tryInsert(k, 0) == InsertStatus::Inserted);
    if (k >= 3) BOOST_REQUIRE(t.erase(k - 3));
  }
  BOOST_CHECK_EQUAL(t.size(), 3u);
  BOOST_CHECK_EQUAL(t.allocator().peakInUse(), 4u);
  BOOST_CHECK_EQUAL(t.allocator().rejectCount(), 0u);
}

// ---------------------------------------------------------------- clear / iteration / custom types

BOOST_AUTO_TEST_CASE(ClearReturnsAllNodes) {
  Table t(16);
  for (uint64_t k = 0; k < 16; ++k) t.insert(k, 0);
  BOOST_CHECK_EQUAL(t.allocator().inUse(), 16u);
  t.clear();
  BOOST_CHECK(t.empty());
  BOOST_CHECK_EQUAL(t.allocator().inUse(), 0u);
  for (uint64_t k = 0; k < 16; ++k) BOOST_CHECK(!t.contains(k));
  BOOST_CHECK(t.insert(3, 3));                             // usable after clear
}

BOOST_AUTO_TEST_CASE(ForEachVisitsEveryEntryOnce) {
  Table t(64);
  for (uint64_t k = 100; k < 150; ++k) t.insert(k, static_cast<int>(k) * 2);
  std::map<uint64_t, int> seen;
  t.for_each([&](uint64_t k, int v) { BOOST_REQUIRE(seen.emplace(k, v).second); });
  BOOST_CHECK_EQUAL(seen.size(), 50u);
  for (auto & [k, v] : seen) BOOST_CHECK_EQUAL(v, static_cast<int>(k) * 2);
}

BOOST_AUTO_TEST_CASE(CustomKeyHashEqual) {
  HashTableTrivial<Key3, Payload, Key3Hash, Key3Equal> t(8);
  int dummy = 0;
  BOOST_CHECK(t.insert(Key3{1, 2, 3}, Payload{7, &dummy}));
  BOOST_CHECK(t.insert(Key3{3, 2, 1}, Payload{8, nullptr}));
  BOOST_CHECK(!t.insert(Key3{1, 2, 3}, Payload{9, nullptr}));
  BOOST_REQUIRE(t.find(Key3{1, 2, 3}));
  BOOST_CHECK_EQUAL(t.find(Key3{1, 2, 3})->id, 7u);
  BOOST_CHECK_EQUAL(t.find(Key3{1, 2, 3})->ptr, &dummy);
  BOOST_CHECK(t.find(Key3{1, 2, 4}) == nullptr);
}

BOOST_AUTO_TEST_CASE(StringKeys) {
  HashTableTrivial<std::string, int> t(8);                 // non-trivial key: destroyed on erase/clear
  t.insert("alpha", 1);
  t.insert(std::string(100, 'x'), 2);
  BOOST_CHECK_EQUAL(*t.find("alpha"), 1);
  BOOST_CHECK_EQUAL(*t.find(std::string(100, 'x')), 2);
  BOOST_CHECK(t.erase("alpha"));
  BOOST_CHECK(!t.contains("alpha"));
}

// ---------------------------------------------------------------- randomized model check

BOOST_AUTO_TEST_CASE(RandomizedAgainstUnorderedMap) {
  constexpr size_t CAPACITY = 512;
  Table t(CAPACITY, 0, PoolExhaustion::ReturnNull);
  std::unordered_map<uint64_t, int> model;
  std::mt19937_64 rng(2026);

  for (int op = 0; op < 200000; ++op) {
    const uint64_t key = rng() % 2048;                     // small key space: many hits and misses
    switch (rng() % 4) {
      case 0:
      case 1: {
        const int value = static_cast<int>(rng() % 1000);
        const InsertStatus st = t.tryInsert(key, value);
        if (model.contains(key)) {
          BOOST_REQUIRE(st == InsertStatus::KeyExists);
        }
        else if (model.size() == CAPACITY) {
          BOOST_REQUIRE(st == InsertStatus::Full);
        }
        else {
          BOOST_REQUIRE(st == InsertStatus::Inserted);
          model.emplace(key, value);
        }
        break;
      }
      case 2:
        BOOST_REQUIRE_EQUAL(t.erase(key), model.erase(key) == 1);
        break;
      case 3: {
        const int * v = t.find(key);
        auto it = model.find(key);
        BOOST_REQUIRE_EQUAL(v != nullptr, it != model.end());
        if (v) BOOST_REQUIRE_EQUAL(*v, it->second);
        break;
      }
    }
    BOOST_REQUIRE_EQUAL(t.size(), model.size());
  }
  size_t visited = 0;
  t.for_each([&](uint64_t k, int v) { ++visited; BOOST_REQUIRE_EQUAL(model.at(k), v); });
  BOOST_CHECK_EQUAL(visited, model.size());
  BOOST_CHECK_EQUAL(t.allocator().inUse(), model.size());
  BOOST_CHECK_EQUAL(t.allocator().fallbackCount(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Byte-array keys with a custom hash functor (merged from the former
// src/test/utility/HashTableTrivialTest.cpp)
// ============================================================================

namespace {
constexpr size_t TEST_KEY_SIZE = 8;
using ByteKey = std::array<std::byte, TEST_KEY_SIZE>;

ByteKey filledKey(int b) {
  ByteKey k;
  std::fill(k.begin(), k.end(), static_cast<std::byte>(b));
  return k;
}

ByteKey indexKey(size_t i) {
  ByteKey k{};
  std::memcpy(k.data(), &i, std::min(sizeof(i), TEST_KEY_SIZE));
  return k;
}
}

namespace {
// custom hasher passed as the Hash template argument (specializing std::hash for
// std::array<std::byte, N>, which is not a program-defined type, would be undefined behaviour)
struct ByteKeyHash {
  size_t operator()(const ByteKey & k) const noexcept {
    size_t seed = 0;
    for (std::byte b : k) {
      seed ^= static_cast<size_t>(b) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
  }
};

using ByteKeyTable = HashTableTrivial<ByteKey, int, ByteKeyHash>;
}

BOOST_AUTO_TEST_SUITE(HashTableTrivialByteKeyTests)

BOOST_AUTO_TEST_CASE(ConstructionAndEmpty) {
  BOOST_CHECK_THROW(ByteKeyTable(0), std::invalid_argument);
  ByteKeyTable ht(10);
  BOOST_CHECK_EQUAL(ht.size(), 0u);
  BOOST_CHECK(ht.empty());
}

BOOST_AUTO_TEST_CASE(InsertAndFind) {
  ByteKeyTable ht(10);
  const ByteKey key1 = filledKey(1), key2 = filledKey(2), key3 = filledKey(3);
  BOOST_CHECK(ht.insert(key1, 100));
  BOOST_CHECK_EQUAL(ht.size(), 1u);
  BOOST_CHECK(!ht.empty());
  BOOST_CHECK(ht.insert(key2, 200));
  BOOST_CHECK(ht.insert(key3, 300));
  BOOST_CHECK_EQUAL(ht.size(), 3u);

  BOOST_CHECK_EQUAL(*ht.find(key1), 100);
  BOOST_CHECK_EQUAL(*ht.find(key2), 200);
  BOOST_CHECK_EQUAL(*ht.find(key3), 300);
  BOOST_CHECK(ht.find(filledKey(99)) == nullptr);

  BOOST_CHECK(!ht.insert(key1, 101));                      // duplicate
  BOOST_CHECK_EQUAL(ht.size(), 3u);
  BOOST_CHECK_EQUAL(*ht.find(key1), 100);
}

BOOST_AUTO_TEST_CASE(Erase) {
  ByteKeyTable ht(10);
  const ByteKey key1 = filledKey(1), key2 = filledKey(2);
  ht.insert(key1, 100);
  ht.insert(key2, 200);
  BOOST_CHECK(ht.erase(key1));
  BOOST_CHECK_EQUAL(ht.size(), 1u);
  BOOST_CHECK(ht.find(key1) == nullptr);
  BOOST_CHECK(!ht.erase(filledKey(99)));
  BOOST_CHECK_EQUAL(ht.size(), 1u);
  BOOST_CHECK(ht.erase(key2));
  BOOST_CHECK(ht.empty());
}

BOOST_AUTO_TEST_CASE(Clear) {
  ByteKeyTable ht(10);
  ht.insert(filledKey(1), 100);
  ht.insert(filledKey(2), 200);
  ht.clear();
  BOOST_CHECK(ht.empty());
  BOOST_CHECK(ht.find(filledKey(1)) == nullptr);
  BOOST_CHECK(ht.find(filledKey(2)) == nullptr);
}

BOOST_AUTO_TEST_CASE(LargeNumberOfElements) {
  constexpr size_t N = 1000;
  ByteKeyTable ht(N);
  for (size_t i = 0; i < N; ++i) {
    BOOST_CHECK_MESSAGE(ht.insert(indexKey(i), static_cast<int>(i)), "Insertion failed for key " << i);
  }
  BOOST_CHECK_EQUAL(ht.size(), N);
  for (size_t i = 0; i < N; ++i) {
    const int * v = ht.find(indexKey(i));
    BOOST_REQUIRE(v);
    BOOST_CHECK_EQUAL(*v, static_cast<int>(i));
  }
  for (size_t i = 0; i < N / 2; ++i) {
    BOOST_CHECK_MESSAGE(ht.erase(indexKey(i)), "Erasure failed for key " << i);
  }
  BOOST_CHECK_EQUAL(ht.size(), N / 2);
  for (size_t i = 0; i < N; ++i) {
    BOOST_CHECK_EQUAL(ht.find(indexKey(i)) != nullptr, i >= N / 2);
  }
  ht.clear();
  BOOST_CHECK(ht.empty());
}

BOOST_AUTO_TEST_SUITE_END()
