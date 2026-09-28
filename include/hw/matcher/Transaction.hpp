#pragma once
#include <variant>
#include <hw/matcher/Types.hpp>

namespace hw::matcher {

//
// Outbound events. Every event carries an engine sequence number, so the
// event stream is totally ordered and can be replayed / diffed.
//

// A single match between a resting (maker) order and an incoming (taker) order.
// Always executes at the maker's price.
struct Trade {
  OrderId   makerOrderId   = 0;
  OrderId   takerOrderId   = 0;
  AccountId makerAccount   = 0;
  AccountId takerAccount   = 0;
  Side      takerSide      = Side::Buy;
  Price     price;
  Quantity  qty            = 0;
  Quantity  makerRemaining = 0;   // after this trade
  Quantity  takerRemaining = 0;   // after this trade
  friend bool operator == (const Trade &, const Trade &) = default;
};

struct OrderAccepted {
  OrderId id = 0;
  friend bool operator == (const OrderAccepted &, const OrderAccepted &) = default;
};

struct OrderRejected {
  OrderId      id     = 0;
  RejectReason reason = RejectReason::None;
  friend bool operator == (const OrderRejected &, const OrderRejected &) = default;
};

// cancel request could not be applied (order not resting)
struct CancelRejected {
  OrderId      id     = 0;
  RejectReason reason = RejectReason::UnknownOrder;
  friend bool operator == (const CancelRejected &, const CancelRejected &) = default;
};

struct OrderCanceled {
  OrderId      id        = 0;
  Quantity     remaining = 0;     // quantity removed from the book / not executed
  CancelReason reason    = CancelReason::UserRequested;
  friend bool operator == (const OrderCanceled &, const OrderCanceled &) = default;
};

// Taker remainder was placed on the book
struct OrderRested {
  OrderId  id        = 0;
  Price    price;
  Quantity remaining = 0;
  friend bool operator == (const OrderRested &, const OrderRested &) = default;
};

using EventBody = std::variant<OrderAccepted, OrderRejected, Trade, OrderCanceled, OrderRested, CancelRejected>;

struct Event {
  SeqNo     seqno = 0;
  EventBody body;
  friend bool operator == (const Event &, const Event &) = default;
};

}
