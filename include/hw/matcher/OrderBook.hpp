#pragma once
#include <algorithm>
#include <cassert>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <hw/matcher/Order.hpp>
#include <hw/matcher/PriceLevel.hpp>
#include <hw/utility/Allocator.hpp>

namespace hw::matcher {

// Aggregated view of one price level
struct LevelInfo {
  Price    price;
  Quantity qty        = 0;
  size_t   orderCount = 0;
};

struct TopOfBook {
  std::optional<LevelInfo> bid;
  std::optional<LevelInfo> ask;
};

struct BookSnapshot {
  std::vector<LevelInfo> bids;   // best (highest) first
  std::vector<LevelInfo> asks;   // best (lowest) first
};

//
// Resting orders for a single market; price-time priority. Heap-free after construction.
//
// - Each side is a std::vector<PriceLevel*> sorted from worst to best price, so the best level
//   is back(): matching at the top and removing an emptied top level (pop_back) are O(1), and
//   inserting a new level near the top shifts only a few pointers. Lookup is a binary search.
// - Both vectors reserve 'maxLevels' at construction and there can never be more levels than
//   the level pool holds, so they never reallocate.
// - PriceLevel objects come from an AllocatorTrivial pool (ReturnNull): when all 'maxLevels'
//   levels are in use, add() fails instead of allocating.
// - Orders are NOT owned: the caller allocates them, the book links them into levels.
// - Not thread safe.
//
class OrderBook {
  using Levels    = std::vector<PriceLevel *>;
  using LevelPool = hw::utility::AllocatorTrivial<PriceLevel>;

public:
  static constexpr size_t DEFAULT_MAX_LEVELS = 4096;

  explicit OrderBook(std::string symbol, size_t maxLevels = DEFAULT_MAX_LEVELS)
    : _symbol(std::move(symbol)), _levelPool(maxLevels, hw::utility::PoolExhaustion::ReturnNull)
  {
    _bids.reserve(maxLevels);
    _asks.reserve(maxLevels);
  }

  ~OrderBook() { clear([](Order &) {}); }

  OrderBook(const OrderBook &) = delete;
  OrderBook & operator = (const OrderBook &) = delete;

  const std::string & symbol()     const noexcept { return _symbol; }
  size_t orderCount()              const noexcept { return _orderCount; }
  size_t levelCount(Side side)     const noexcept { return levels(side).size(); }
  size_t levelCapacity()           const noexcept { return _levelPool.capacity(); }
  size_t freeLevels()              const noexcept { return _levelPool.available(); }
  bool   empty(Side side)          const noexcept { return levels(side).empty(); }

  // rests a limit order at the back of its price level;
  // returns false (book unchanged) if a new level is needed and the level pool is exhausted
  bool add(Order & order) {
    assert(order.type == OrderType::Limit && order.remaining() > 0 && !order.isResting());
    Levels & side = levels(order.side);
    auto pos = position(order.side, order.price);
    PriceLevel * level = nullptr;
    if (pos != side.end() && (*pos)->price() == order.price) {
      level = *pos;
    }
    else {
      level = _levelPool.create(order.price);
      if (!level) [[unlikely]] return false;
      assert(side.size() < side.capacity());
      side.insert(pos, level);                   // within reserved capacity: no reallocation
    }
    level->append(order);
    ++_orderCount;
    return true;
  }

  // removes a resting order from the book (cancel / self-trade prevention)
  void remove(Order & order) noexcept {
    PriceLevel * level = order.level;
    assert(level);
    level->erase(order);
    --_orderCount;
    if (level->empty()) dropLevel(order.side, level);
  }

  // oldest order at the best price on 'side', or nullptr if the side is empty
  Order * bestOrder(Side side) noexcept {
    Levels & lv = levels(side);
    return lv.empty() ? nullptr : &lv.back()->front();
  }
  const Order * bestOrder(Side side) const noexcept {
    const Levels & lv = levels(side);
    return lv.empty() ? nullptr : &lv.back()->front();
  }

  // fills a resting order; a fully filled order is removed from the book (still owned by the caller)
  void fill(Order & order, Quantity qty) noexcept {
    PriceLevel * level = order.level;
    assert(level);
    level->fill(order, qty);
    if (order.remaining() == 0) {
      level->erase(order);
      --_orderCount;
      if (level->empty()) dropLevel(order.side, level);
    }
  }

  std::optional<Price> bestPrice(Side side) const noexcept {
    const Levels & lv = levels(side);
    if (lv.empty()) return std::nullopt;
    return lv.back()->price();
  }

  TopOfBook top() const {
    return TopOfBook{ bestLevel(Side::Buy), bestLevel(Side::Sell) };
  }

  BookSnapshot depth(size_t maxLevels = std::numeric_limits<size_t>::max()) const {
    return BookSnapshot{ snapshot(Side::Buy, maxLevels), snapshot(Side::Sell, maxLevels) };
  }

  // visits resting orders on 'side' in priority order (best price first, FIFO within a level)
  template <typename Fn>
  void forEachOrder(Side side, Fn && fn) const {
    const Levels & lv = levels(side);
    for (auto it = lv.rbegin(); it != lv.rend(); ++it) {
      for (const Order & o : **it) fn(o);
    }
  }

  // unlinks every resting order (passing each to onOrder, e.g. to free it) and frees all levels
  template <typename Fn>
  void clear(Fn && onOrder) {
    for (Levels * lv : {&_bids, &_asks}) {
      for (PriceLevel * level : *lv) {
        while (!level->empty()) {
          Order & o = level->front();
          level->erase(o);
          onOrder(o);
        }
        _levelPool.release(level);
      }
      lv->clear();
    }
    _orderCount = 0;
  }

private:
  Levels &       levels(Side side)       noexcept { return side == Side::Buy ? _bids : _asks; }
  const Levels & levels(Side side) const noexcept { return side == Side::Buy ? _bids : _asks; }

  // true if price 'a' has priority over price 'b' on 'side'
  static bool better(Side side, const Price & a, const Price & b) noexcept {
    return side == Side::Buy ? a > b : a < b;
  }

  // first level (in worst-to-best order) that is not worse than 'price'
  Levels::iterator position(Side side, const Price & price) noexcept {
    Levels & lv = levels(side);
    // fast path: price at or better than the current best (most common near the top of book)
    if (lv.empty() || better(side, price, lv.back()->price())) return lv.end();
    if (lv.back()->price() == price) return lv.end() - 1;
    return std::lower_bound(lv.begin(), lv.end(), price,
      [side](const PriceLevel * level, const Price & p) { return better(side, p, level->price()); });
  }

  void dropLevel(Side side, PriceLevel * level) noexcept {
    Levels & lv = levels(side);
    if (lv.back() == level) {
      lv.pop_back();                             // top of book: O(1)
    }
    else {
      auto pos = position(side, level->price());
      assert(pos != lv.end() && *pos == level);
      lv.erase(pos);
    }
    _levelPool.release(level);
  }

  static LevelInfo info(const PriceLevel & level) noexcept {
    return LevelInfo{ level.price(), level.totalQty(), level.orderCount() };
  }

  std::optional<LevelInfo> bestLevel(Side side) const {
    const Levels & lv = levels(side);
    if (lv.empty()) return std::nullopt;
    return info(*lv.back());
  }

  std::vector<LevelInfo> snapshot(Side side, size_t maxLevels) const {
    const Levels & lv = levels(side);
    std::vector<LevelInfo> result;
    result.reserve(std::min(maxLevels, lv.size()));
    for (auto it = lv.rbegin(); it != lv.rend() && result.size() < maxLevels; ++it) {
      result.push_back(info(**it));
    }
    return result;
  }

  std::string _symbol;
  LevelPool   _levelPool;
  Levels      _bids;          // worst (lowest) .. best (highest) at back
  Levels      _asks;          // worst (highest) .. best (lowest) at back
  size_t      _orderCount = 0;
};

}
