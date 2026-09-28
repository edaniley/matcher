#pragma once
#include <variant>
#include <hw/matcher/Types.hpp>

namespace hw::matcher {

//
// Inbound requests. The engine consumes these strictly in arrival order;
// the order of requests is the only input that determines the output.
//
struct NewOrder {
  OrderId     id      = 0;
  AccountId   account = 0;
  Side        side    = Side::Buy;
  OrderType   type    = OrderType::Limit;
  TimeInForce tif     = TimeInForce::GTC;
  Price       price;          // value + decimals; ignored for market orders
  Quantity    size    = 0;
};

struct CancelOrder {
  OrderId id = 0;
};

using Request = std::variant<NewOrder, CancelOrder>;

}
