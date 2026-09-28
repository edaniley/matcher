#pragma once
#include <cassert>
#include <hw/matcher/Types.hpp>
#include <hw/utility/DList.hpp>

namespace hw::matcher {

class PriceLevel;

//
// Order state as tracked by the engine. No heap members.
// Engine-owned orders live in a pool and are linked into intrusive lists through 'hook':
// the FIFO queue of their PriceLevel while resting, or the engine's retired list once finished.
// Copies (e.g. returned by status queries) are plain values: the hook of a copy is unlinked.
//
struct Order {
  OrderId     id      = 0;
  AccountId   account = 0;
  Side        side    = Side::Buy;
  OrderType   type    = OrderType::Limit;
  TimeInForce tif     = TimeInForce::GTC;
  Price       price;                        // meaningless for market orders
  Quantity    size    = 0;                  // original size
  Quantity    filled  = 0;                  // cumulative filled quantity
  OrderStatus status  = OrderStatus::New;
  SeqNo       seqno   = 0;                  // assigned on acceptance; time priority

  hw::utility::DListHook<Order> hook;       // price level queue (resting) or retired list (finished)
  PriceLevel * level = nullptr;             // resting level; nullptr when not on the book

  Quantity remaining() const noexcept { return size - filled; }
  bool isBuy()     const noexcept { return side == Side::Buy; }
  bool isMarket()  const noexcept { return type == OrderType::Market; }
  bool isResting() const noexcept { return level != nullptr; }
  bool isOpen()    const noexcept {
    return status == OrderStatus::New || status == OrderStatus::PartiallyFilled;
  }

  // true if this order is willing to trade against a resting order at 'bookPrice'
  bool crosses(Price bookPrice) const noexcept {
    if (isMarket()) return true;
    return isBuy() ? price >= bookPrice : price <= bookPrice;
  }

  void fill(Quantity qty) noexcept {
    assert(qty > 0 && qty <= remaining());
    filled += qty;
    status = remaining() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
  }

  void cancel() noexcept { status = OrderStatus::Canceled; }

  // compares order state; list membership (hook, level) is not part of the value
  friend bool operator == (const Order & a, const Order & b) noexcept {
    return a.id == b.id && a.account == b.account && a.side == b.side && a.type == b.type &&
           a.tif == b.tif && a.price == b.price && a.size == b.size && a.filled == b.filled &&
           a.status == b.status && a.seqno == b.seqno;
  }
};

}
