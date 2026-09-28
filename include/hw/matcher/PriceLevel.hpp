#pragma once
#include <cassert>
#include <hw/matcher/Order.hpp>
#include <hw/utility/DList.hpp>

namespace hw::matcher {

//
// FIFO queue of resting orders at a single price.
// - Orders are linked intrusively (Order::hook): append / erase are O(1), no allocation.
// - Orders are not owned: the caller (engine) allocates and frees them.
// - Tracks total resting quantity so aggregated book queries are O(1) per level.
// - Not copyable or movable (orders point back at their level); allocate levels from a pool.
//
class PriceLevel {
public:
  using Queue          = hw::utility::DList<Order, &Order::hook>;
  using iterator       = Queue::iterator;
  using const_iterator = Queue::const_iterator;

  explicit PriceLevel(Price price) noexcept : _price(price) {}
  ~PriceLevel() {
    for (Order & o : _orders) o.level = nullptr;
    _orders.clear();
  }

  PriceLevel(const PriceLevel &) = delete;
  PriceLevel & operator = (const PriceLevel &) = delete;

  Price    price()      const noexcept { return _price; }
  Quantity totalQty()   const noexcept { return _totalQty; }
  size_t   orderCount() const noexcept { return _orders.size(); }
  bool     empty()      const noexcept { return _orders.empty(); }

  Order &       front()       noexcept { assert(!empty()); return _orders.front(); }
  const Order & front() const noexcept { assert(!empty()); return _orders.front(); }

  // appends at the back of the queue (lowest time priority at this price)
  void append(Order & order) noexcept {
    assert(order.price == _price && order.remaining() > 0 && !order.isResting());
    order.level = this;
    _totalQty += order.remaining();
    _orders.push_back(order);
  }

  // partial or full fill of a resting order; priority is preserved (caller removes a filled order)
  void fill(Order & order, Quantity qty) noexcept {
    assert(order.level == this);
    order.fill(qty);
    _totalQty -= qty;
  }

  // unlinks order from the queue
  void erase(Order & order) noexcept {
    assert(order.level == this);
    _totalQty -= order.remaining();
    _orders.erase(order);
    order.level = nullptr;
  }

  iterator       begin()       noexcept { return _orders.begin(); }
  iterator       end()         noexcept { return _orders.end(); }
  const_iterator begin() const noexcept { return _orders.begin(); }
  const_iterator end()   const noexcept { return _orders.end(); }

private:
  Price    _price;
  Quantity _totalQty = 0;
  Queue    _orders;
};

}
