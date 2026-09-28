// Verifies the engine's critical path never allocates from the heap.
//
// This translation unit replaces the global operator new / delete for the MatcherTest binary.
// Allocations are only counted while HeapCounter::active is set, so the test framework and
// setup code are unaffected. Any std container growing (including std::vector past its
// reservation) goes through operator new and is therefore caught.
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#include <new>
#include <hw/matcher/MatchingEngine.hpp>
#include "TestHelpers.hpp"

namespace {

struct HeapCounter {
  static inline thread_local bool   active = false;
  static inline thread_local size_t count  = 0;
};

void * countedAlloc(std::size_t size, std::size_t alignment = 0) {
  if (HeapCounter::active) ++HeapCounter::count;
  if (size == 0) size = 1;
  void * p = nullptr;
  if (alignment > alignof(std::max_align_t)) {
    if (posix_memalign(&p, alignment, size) != 0) p = nullptr;
  }
  else {
    p = std::malloc(size);
  }
  if (!p) throw std::bad_alloc();
  return p;
}

}

void * operator new  (std::size_t n)                                   { return countedAlloc(n); }
void * operator new[](std::size_t n)                                   { return countedAlloc(n); }
void * operator new  (std::size_t n, std::align_val_t a)               { return countedAlloc(n, static_cast<std::size_t>(a)); }
void * operator new[](std::size_t n, std::align_val_t a)               { return countedAlloc(n, static_cast<std::size_t>(a)); }
void * operator new  (std::size_t n, const std::nothrow_t &) noexcept  { try { return countedAlloc(n); } catch (...) { return nullptr; } }
void * operator new[](std::size_t n, const std::nothrow_t &) noexcept  { try { return countedAlloc(n); } catch (...) { return nullptr; } }
void operator delete  (void * p) noexcept                              { std::free(p); }
void operator delete[](void * p) noexcept                              { std::free(p); }
void operator delete  (void * p, std::size_t) noexcept                 { std::free(p); }
void operator delete[](void * p, std::size_t) noexcept                 { std::free(p); }
void operator delete  (void * p, std::align_val_t) noexcept            { std::free(p); }
void operator delete[](void * p, std::align_val_t) noexcept            { std::free(p); }
void operator delete  (void * p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void * p, std::size_t, std::align_val_t) noexcept { std::free(p); }

using namespace hw::matcher;
using namespace hw::matcher::test;

namespace {

// event sink that does not allocate
struct CountingSink {
  size_t events = 0;
  size_t trades = 0;
  size_t capacity = 0;
  void operator () (const Event & e) noexcept {
    ++events;
    if (std::holds_alternative<Trade>(e.body)) ++trades;
    if (auto r = std::get_if<OrderRejected>(&e.body); r && r->reason == RejectReason::CapacityExceeded) ++capacity;
    if (auto c = std::get_if<OrderCanceled>(&e.body); c && c->reason == CancelReason::CapacityExceeded) ++capacity;
  }
};

// processes 'requests' with allocation counting on; returns number of heap allocations
size_t allocationsWhileProcessing(MatchingEngine & engine, const std::vector<Request> & requests, CountingSink & sink) {
  HeapCounter::count  = 0;
  HeapCounter::active = true;
  for (const auto & req : requests) engine.process(req, sink);
  HeapCounter::active = false;
  return HeapCounter::count;
}

}

BOOST_AUTO_TEST_SUITE(NoHeapTests)

BOOST_AUTO_TEST_CASE(CounterDetectsAllocations) {
  // self-check of the instrumentation
  HeapCounter::count  = 0;
  HeapCounter::active = true;
  { std::vector<int> v(10); (void)v; }
  HeapCounter::active = false;
  BOOST_CHECK_EQUAL(HeapCounter::count, 1u);
}

BOOST_AUTO_TEST_CASE(ProcessDoesNotAllocate) {
  const auto requests = randomRequests(4242, 50000);           // generated before counting starts
  MatchingEngine engine(EngineConfig{.symbol = "BTC", .selfTradePolicy = SelfTradePolicy::CancelOldest});
  CountingSink sink;
  const size_t allocations = allocationsWhileProcessing(engine, requests, sink);
  BOOST_CHECK_EQUAL(allocations, 0u);
  BOOST_CHECK_GT(sink.trades, 1000u);                            // the run did real matching work
}

BOOST_AUTO_TEST_CASE(ProcessDoesNotAllocateAtCapacityLimits) {
  // tiny pools: order-pool rejects, level-pool cancels, and retention evictions on every other request
  for (size_t retained : {size_t(0), size_t(4), std::numeric_limits<size_t>::max()}) {
    BOOST_TEST_CONTEXT("retained=" << retained) {
      const auto requests = randomRequests(99, 20000);
      MatchingEngine engine(EngineConfig{.symbol = "BTC", .maxOrders = 32, .maxLevels = 6, .retainedOrders = retained});
      CountingSink sink;
      const size_t allocations = allocationsWhileProcessing(engine, requests, sink);
      BOOST_CHECK_EQUAL(allocations, 0u);
      BOOST_CHECK_GT(sink.capacity, 0u);                         // limits were actually hit
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()
