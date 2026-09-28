#pragma once
#include <algorithm>
#include <cassert>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <hw/matcher/OrderBook.hpp>
#include <hw/matcher/Request.hpp>
#include <hw/matcher/Transaction.hpp>
#include <hw/utility/Allocator.hpp>
#include <hw/utility/DList.hpp>
#include <hw/utility/HashTableTrivial.hpp>

namespace hw::matcher {

struct EngineConfig {
  std::string     symbol;
  SelfTradePolicy selfTradePolicy = SelfTradePolicy::CancelNewest;
  size_t          maxOrders       = 1 << 16;   // order pool: resting + retained finished orders
  size_t          maxLevels       = OrderBook::DEFAULT_MAX_LEVELS;   // price levels, both sides together
  size_t          retainedOrders  = std::numeric_limits<size_t>::max();   // cap on finished orders kept for queries
};

// Sink receives every event, in order; any callable taking const Event &
template <typename Sink>
concept EventSink = std::invocable<Sink &, const Event &>;

//
// MatchingEngine: continuous limit order book for a single market (single-threaded actor).
//
// - process() is the only way to mutate state; requests are applied strictly one at a time.
// - Deterministic: output depends only on the request sequence. No clocks, no randomness,
//   no floating point; time priority and event order come from engine sequence numbers.
// - Price-time priority; trades execute at the resting (maker) order's price.
// - Event order for a new order: OrderAccepted, then Trade / OrderCanceled(SelfTrade) as
//   matching proceeds, then at most one of OrderRested / OrderCanceled for the remainder.
//   Invalid requests produce only OrderRejected / CancelRejected.
// - Market orders never rest: they must be IOC; any unfilled remainder is canceled.
// - Self-trade prevention (orders of the same account would match):
//     CancelNewest: incoming order's remainder is canceled; resting order untouched
//     CancelOldest: resting order is canceled; incoming order continues matching
//     CancelBoth:   both are canceled
//
// Memory: no heap allocation while processing requests. Everything is allocated in the constructor:
// - Orders come from a pool of 'maxOrders' (AllocatorTrivial, ReturnNull); an id index
//   (HashTableTrivial, same capacity) maps order id -> Order*.
// - Finished orders (filled / canceled / rejected) stay in the pool, linked into a FIFO 'retired'
//   list, so status() and duplicate-id detection keep working. The oldest retired order is evicted
//   when more than 'retainedOrders' are retained, or when a new order needs its pool slot.
//   Evicted ids are forgotten: status() returns nothing and the id may be reused.
// - If the pool holds only resting orders (nothing to evict), a new order is rejected with
//   RejectReason::CapacityExceeded. If a remainder cannot rest because all 'maxLevels' price
//   levels are in use, it is canceled with CancelReason::CapacityExceeded.
//
class MatchingEngine {
  using OrderPool   = hw::utility::AllocatorTrivial<Order>;
  using OrderIndex  = hw::utility::HashTableTrivial<OrderId, Order *>;
  using RetiredList = hw::utility::DList<Order, &Order::hook>;

public:
  explicit MatchingEngine(EngineConfig config)
    : _config(std::move(config)),
      _orders(_config.maxOrders, hw::utility::PoolExhaustion::ReturnNull),
      _index(std::max<size_t>(_config.maxOrders, 1), 0, hw::utility::PoolExhaustion::ReturnNull),
      _book(_config.symbol, _config.maxLevels)
  {
  }

  ~MatchingEngine() {
    _book.clear([this](Order & o) { _orders.release(&o); });
    while (!_retired.empty()) {
      Order & o = _retired.front();
      _retired.pop_front();
      _orders.release(&o);
    }
    _index.clear();
  }

  MatchingEngine(const MatchingEngine &) = delete;
  MatchingEngine & operator = (const MatchingEngine &) = delete;

  const EngineConfig & config() const noexcept { return _config; }
  const OrderBook &    book()   const noexcept { return _book; }
  SeqNo                lastEventSeqNo() const noexcept { return _eventSeqNo; }
  size_t               retainedCount()  const noexcept { return _retired.size(); }
  const OrderPool &    orderPool()      const noexcept { return _orders; }

  template <EventSink Sink>
  void process(const Request & request, Sink && sink) {
    std::visit([&](const auto & req) { process(req, sink); }, request);
  }

  template <EventSink Sink>
  void process(const NewOrder & req, Sink && sink) {
    if (_index.contains(req.id)) {
      emit(sink, OrderRejected{req.id, RejectReason::DuplicateOrderId});
      return;
    }
    const RejectReason invalid = validate(req);
    Order * order = newOrder(req);
    if (!order) [[unlikely]] {
      emit(sink, OrderRejected{req.id, invalid != RejectReason::None ? invalid : RejectReason::CapacityExceeded});
      return;
    }
    if (invalid != RejectReason::None) {
      order->status = OrderStatus::Rejected;
      retire(*order);
      emit(sink, OrderRejected{req.id, invalid});
      return;
    }

    Order & taker = *order;
    taker.seqno = ++_orderSeqNo;
    emit(sink, OrderAccepted{taker.id});

    const Side makerSide = opposite(taker.side);
    bool takerCanceled = false;

    while (taker.remaining() > 0) {
      Order * maker = _book.bestOrder(makerSide);
      if (!maker || !taker.crosses(maker->price)) break;

      if (maker->account == taker.account) {
        const SelfTradePolicy policy = _config.selfTradePolicy;
        if (policy == SelfTradePolicy::CancelOldest || policy == SelfTradePolicy::CancelBoth) {
          cancelResting(*maker, CancelReason::SelfTrade, sink);
        }
        if (policy == SelfTradePolicy::CancelNewest || policy == SelfTradePolicy::CancelBoth) {
          takerCanceled = true;
          emit(sink, OrderCanceled{taker.id, taker.remaining(), CancelReason::SelfTrade});
          taker.cancel();
          break;
        }
        continue;
      }

      const Quantity qty = std::min(taker.remaining(), maker->remaining());
      taker.fill(qty);
      _book.fill(*maker, qty);                   // a fully filled maker leaves the book
      emit(sink, Trade{
        .makerOrderId   = maker->id,
        .takerOrderId   = taker.id,
        .makerAccount   = maker->account,
        .takerAccount   = taker.account,
        .takerSide      = taker.side,
        .price          = maker->price,
        .qty            = qty,
        .makerRemaining = maker->remaining(),
        .takerRemaining = taker.remaining(),
      });
      if (maker->remaining() == 0) retire(*maker);
    }

    if (takerCanceled || taker.remaining() == 0) {
      retire(taker);
    }
    else if (taker.isMarket() || taker.tif == TimeInForce::IOC) {
      emit(sink, OrderCanceled{taker.id, taker.remaining(), CancelReason::IOCRemainder});
      taker.cancel();
      retire(taker);
    }
    else if (_book.add(taker)) {
      emit(sink, OrderRested{taker.id, taker.price, taker.remaining()});
    }
    else [[unlikely]] {
      emit(sink, OrderCanceled{taker.id, taker.remaining(), CancelReason::CapacityExceeded});
      taker.cancel();
      retire(taker);
    }
  }

  template <EventSink Sink>
  void process(const CancelOrder & req, Sink && sink) {
    Order * const * found = _index.find(req.id);
    if (!found || !(*found)->isResting()) {
      emit(sink, CancelRejected{req.id, RejectReason::UnknownOrder});
      return;
    }
    cancelResting(**found, CancelReason::UserRequested, sink);
  }

  // --- queries (read-only) ---

  TopOfBook top() const { return _book.top(); }

  BookSnapshot depth(size_t maxLevels = std::numeric_limits<size_t>::max()) const {
    return _book.depth(maxLevels);
  }

  // current state of an order the engine knows: resting, or finished and still retained
  std::optional<Order> status(OrderId id) const {
    Order * const * found = _index.find(id);
    if (!found) return std::nullopt;
    return **found;
  }

private:
  RejectReason validate(const NewOrder & req) const noexcept {
    if (req.size <= 0) return RejectReason::InvalidSize;
    if (req.type == OrderType::Market) {
      if (req.tif != TimeInForce::IOC) return RejectReason::InvalidTimeInForce;
    }
    else if (!req.price.isValid() || !req.price.isPositive()) {
      return RejectReason::InvalidPrice;
    }
    return RejectReason::None;
  }

  // allocates and indexes an order for 'req'; evicts the oldest retired order if the pool is full.
  // Returns nullptr if the pool holds only resting orders.
  Order * newOrder(const NewOrder & req) {
    Order * order = _orders.allocate();
    if (!order && !_retired.empty()) {
      evictOldest();
      order = _orders.allocate();
    }
    if (!order) return nullptr;
    _orders.construct(order);
    order->id      = req.id;
    order->account = req.account;
    order->side    = req.side;
    order->type    = req.type;
    order->tif     = req.tif;
    order->price   = req.type == OrderType::Market ? Price{} : req.price;
    order->size    = req.size;
    [[maybe_unused]] const auto st = _index.tryInsert(order->id, order);
    assert(st == hw::utility::InsertStatus::Inserted);   // index capacity == pool capacity
    return order;
  }

  // finished order: keep it for status queries, bounded by retainedOrders
  void retire(Order & order) noexcept {
    assert(!order.isResting() && !order.hook.isLinked());
    _retired.push_back(order);
    if (_retired.size() > _config.retainedOrders) evictOldest();
  }

  void evictOldest() noexcept {
    Order & oldest = _retired.front();
    _retired.pop_front();
    _index.erase(oldest.id);
    _orders.release(&oldest);
  }

  template <typename Sink>
  void cancelResting(Order & order, CancelReason reason, Sink && sink) {
    const Quantity remaining = order.remaining();
    _book.remove(order);
    order.cancel();
    emit(sink, OrderCanceled{order.id, remaining, reason});
    retire(order);                               // may release the order: emit first
  }

  template <typename Sink, typename Body>
  void emit(Sink && sink, Body && body) {
    sink(Event{++_eventSeqNo, EventBody{std::forward<Body>(body)}});
  }

  EngineConfig _config;
  OrderPool    _orders;        // declared before users so it is destroyed last
  OrderIndex   _index;         // order id -> Order* (resting and retained orders)
  RetiredList  _retired;       // finished orders, oldest first
  OrderBook    _book;
  SeqNo        _orderSeqNo = 0;
  SeqNo        _eventSeqNo = 0;
};

}
