#pragma once
#include <cstdint>
#include <string_view>
#include <hw/matcher/Price.hpp>

namespace hw::matcher {

//
// Scalar types. Price is a fixed-point decimal (value + decimals, see Price.hpp);
// quantities are integer lots. No floating point, so matching is exact and deterministic.
//
using OrderId   = uint64_t;
using AccountId = uint64_t;
using Quantity  = int64_t;    // size in lots
using SeqNo     = uint64_t;   // engine-assigned; defines time priority and event order

enum class Side : uint8_t { Buy, Sell };

enum class OrderType : uint8_t { Limit, Market };

// GTC: rest unfilled remainder on the book
// IOC: match what is possible immediately, cancel the remainder
enum class TimeInForce : uint8_t { GTC, IOC };

enum class OrderStatus : uint8_t { New, PartiallyFilled, Filled, Canceled, Rejected };

enum class RejectReason : uint8_t {
  None,
  DuplicateOrderId,
  InvalidSize,
  InvalidPrice,
  InvalidTimeInForce,   // e.g. market order with GTC
  UnknownOrder,         // cancel of an order that is not resting
  CapacityExceeded,     // engine order pool exhausted (see EngineConfig::maxOrders)
};

enum class CancelReason : uint8_t {
  UserRequested,
  IOCRemainder,         // unfilled part of an IOC / market order
  SelfTrade,            // removed by self-trade prevention
  CapacityExceeded,     // remainder could not rest: no free price level (see EngineConfig::maxLevels)
};

// Self-trade prevention policy: which side of a would-be self match is canceled
enum class SelfTradePolicy : uint8_t { CancelNewest, CancelOldest, CancelBoth };

constexpr Side opposite(Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

constexpr std::string_view toString(Side v) noexcept {
  switch (v) {
    case Side::Buy:  return "BUY";
    case Side::Sell: return "SELL";
  }
  return "?";
}

constexpr std::string_view toString(OrderType v) noexcept {
  switch (v) {
    case OrderType::Limit:  return "LIMIT";
    case OrderType::Market: return "MARKET";
  }
  return "?";
}

constexpr std::string_view toString(TimeInForce v) noexcept {
  switch (v) {
    case TimeInForce::GTC: return "GTC";
    case TimeInForce::IOC: return "IOC";
  }
  return "?";
}

constexpr std::string_view toString(OrderStatus v) noexcept {
  switch (v) {
    case OrderStatus::New:             return "NEW";
    case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
    case OrderStatus::Filled:          return "FILLED";
    case OrderStatus::Canceled:        return "CANCELED";
    case OrderStatus::Rejected:        return "REJECTED";
  }
  return "?";
}

constexpr std::string_view toString(RejectReason v) noexcept {
  switch (v) {
    case RejectReason::None:               return "NONE";
    case RejectReason::DuplicateOrderId:   return "DUPLICATE_ORDER_ID";
    case RejectReason::InvalidSize:        return "INVALID_SIZE";
    case RejectReason::InvalidPrice:       return "INVALID_PRICE";
    case RejectReason::InvalidTimeInForce: return "INVALID_TIME_IN_FORCE";
    case RejectReason::UnknownOrder:       return "UNKNOWN_ORDER";
    case RejectReason::CapacityExceeded:   return "CAPACITY_EXCEEDED";
  }
  return "?";
}

constexpr std::string_view toString(CancelReason v) noexcept {
  switch (v) {
    case CancelReason::UserRequested: return "USER_REQUESTED";
    case CancelReason::IOCRemainder:  return "IOC_REMAINDER";
    case CancelReason::SelfTrade:     return "SELF_TRADE";
    case CancelReason::CapacityExceeded: return "CAPACITY_EXCEEDED";
  }
  return "?";
}

constexpr std::string_view toString(SelfTradePolicy v) noexcept {
  switch (v) {
    case SelfTradePolicy::CancelNewest: return "CANCEL_NEWEST";
    case SelfTradePolicy::CancelOldest: return "CANCEL_OLDEST";
    case SelfTradePolicy::CancelBoth:   return "CANCEL_BOTH";
  }
  return "?";
}

}
