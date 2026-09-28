#pragma once
//
// JSON <-> matcher types, used by the replay harness and tests.
// Requires Boost.JSON; exactly one translation unit must include <boost/json/src.hpp>.
//
// Input commands (one JSON object per line):
//   {"op":"new","id":1,"account":7,"side":"buy","type":"limit","tif":"gtc","price":6543210,"decimals":2,"size":5}
//       type defaults to "limit", tif to "gtc", decimals to 0; price is required for limit orders
//   {"op":"cancel","id":1}
//   {"op":"top"}   {"op":"depth","levels":5}   {"op":"status","id":1}
//   {"op":"config","symbol":"BTC","stp":"cancel_oldest","snapshot":"top",
//    "max_orders":65536,"max_levels":4096,"retained_orders":1000}              (before any order)
//
// Output records: keys are always emitted in a fixed order so output is byte-for-byte reproducible.
//
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <boost/json.hpp>
#include <hw/matcher/MatchingEngine.hpp>

namespace hw::matcher::json {

namespace bj = boost::json;

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

enum class SnapshotMode : uint8_t { None, Top, Depth };

// --- commands --------------------------------------------------------------

struct TopQuery    {};
struct DepthQuery  { size_t levels = std::numeric_limits<size_t>::max(); };
struct StatusQuery { OrderId id = 0; };
struct ConfigCmd {
  std::optional<std::string>     symbol;
  std::optional<SelfTradePolicy> selfTradePolicy;
  std::optional<SnapshotMode>    snapshot;
  std::optional<size_t>          maxOrders;
  std::optional<size_t>          maxLevels;
  std::optional<size_t>          retainedOrders;
};

using Command = std::variant<NewOrder, CancelOrder, TopQuery, DepthQuery, StatusQuery, ConfigCmd>;

// --- enum names (lower case on the wire) -------------------------------------

inline std::string lower(std::string_view s) {
  std::string out(s);
  for (char & c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

template <typename Enum>
std::string name(Enum v) { return lower(toString(v)); }

inline std::string_view toString(SnapshotMode v) noexcept {
  switch (v) {
    case SnapshotMode::None:  return "none";
    case SnapshotMode::Top:   return "top";
    case SnapshotMode::Depth: return "depth";
  }
  return "?";
}

template <typename Enum>
Enum parseEnum(std::string_view field, std::string_view text, std::initializer_list<Enum> values) {
  const std::string key = lower(text);
  for (Enum v : values) {
    if (name(v) == key) return v;
  }
  throw ParseError("invalid value '" + std::string(text) + "' for '" + std::string(field) + "'");
}

inline Side parseSide(std::string_view s) {
  return parseEnum("side", s, {Side::Buy, Side::Sell});
}
inline OrderType parseOrderType(std::string_view s) {
  return parseEnum("type", s, {OrderType::Limit, OrderType::Market});
}
inline TimeInForce parseTif(std::string_view s) {
  return parseEnum("tif", s, {TimeInForce::GTC, TimeInForce::IOC});
}
inline SelfTradePolicy parseStp(std::string_view s) {
  return parseEnum("stp", s, {SelfTradePolicy::CancelNewest, SelfTradePolicy::CancelOldest, SelfTradePolicy::CancelBoth});
}
inline SnapshotMode parseSnapshot(std::string_view s) {
  return parseEnum("snapshot", s, {SnapshotMode::None, SnapshotMode::Top, SnapshotMode::Depth});
}

// --- field access -------------------------------------------------------------

namespace detail {

  inline void checkKeys(const bj::object & obj, std::initializer_list<std::string_view> allowed) {
    for (const auto & kv : obj) {
      bool known = false;
      for (auto k : allowed) known = known || kv.key() == k;
      if (!known) throw ParseError("unknown field '" + std::string(kv.key()) + "'");
    }
  }

  inline const bj::value * find(const bj::object & obj, std::string_view key) {
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->value();
  }

  inline int64_t toInt(std::string_view key, const bj::value & v) {
    if (v.is_int64())  return v.get_int64();
    if (v.is_uint64()) {
      if (v.get_uint64() > static_cast<uint64_t>(INT64_MAX)) throw ParseError("'" + std::string(key) + "' out of range");
      return static_cast<int64_t>(v.get_uint64());
    }
    throw ParseError("'" + std::string(key) + "' must be an integer");
  }

  inline uint64_t toUint(std::string_view key, const bj::value & v) {
    if (v.is_uint64()) return v.get_uint64();
    if (v.is_int64() && v.get_int64() >= 0) return static_cast<uint64_t>(v.get_int64());
    throw ParseError("'" + std::string(key) + "' must be a non-negative integer");
  }

  inline std::string_view toStr(std::string_view key, const bj::value & v) {
    if (!v.is_string()) throw ParseError("'" + std::string(key) + "' must be a string");
    return v.get_string();
  }

  inline const bj::value & required(const bj::object & obj, std::string_view key) {
    const bj::value * v = find(obj, key);
    if (!v) throw ParseError("missing field '" + std::string(key) + "'");
    return *v;
  }

}

// --- parsing ------------------------------------------------------------------

inline Command parseCommand(const bj::object & obj) {
  using namespace detail;
  const std::string_view op = toStr("op", required(obj, "op"));

  if (op == "new") {
    checkKeys(obj, {"op", "id", "account", "side", "type", "tif", "price", "decimals", "size"});
    NewOrder req;
    req.id      = toUint("id", required(obj, "id"));
    req.account = toUint("account", required(obj, "account"));
    req.side    = parseSide(toStr("side", required(obj, "side")));
    if (auto v = find(obj, "type")) req.type = parseOrderType(toStr("type", *v));
    if (auto v = find(obj, "tif"))  req.tif  = parseTif(toStr("tif", *v));
    req.size = toInt("size", required(obj, "size"));

    const bj::value * price = find(obj, "price");
    const bj::value * decimals = find(obj, "decimals");
    if (req.type == OrderType::Limit && !price) throw ParseError("missing field 'price' for limit order");
    if (price) {
      int64_t d = decimals ? toInt("decimals", *decimals) : 0;
      if (d < 0 || d > UINT8_MAX) throw ParseError("'decimals' out of range");
      req.price = Price(toInt("price", *price), static_cast<uint8_t>(d));
    }
    else if (decimals) {
      throw ParseError("'decimals' given without 'price'");
    }
    return req;
  }
  if (op == "cancel") {
    checkKeys(obj, {"op", "id"});
    return CancelOrder{toUint("id", required(obj, "id"))};
  }
  if (op == "top") {
    checkKeys(obj, {"op"});
    return TopQuery{};
  }
  if (op == "depth") {
    checkKeys(obj, {"op", "levels"});
    DepthQuery q;
    if (auto v = find(obj, "levels")) q.levels = toUint("levels", *v);
    return q;
  }
  if (op == "status") {
    checkKeys(obj, {"op", "id"});
    return StatusQuery{toUint("id", required(obj, "id"))};
  }
  if (op == "config") {
    checkKeys(obj, {"op", "symbol", "stp", "snapshot", "max_orders", "max_levels", "retained_orders"});
    ConfigCmd cfg;
    if (auto v = find(obj, "symbol"))   cfg.symbol = std::string(toStr("symbol", *v));
    if (auto v = find(obj, "stp"))      cfg.selfTradePolicy = parseStp(toStr("stp", *v));
    if (auto v = find(obj, "snapshot")) cfg.snapshot = parseSnapshot(toStr("snapshot", *v));
    if (auto v = find(obj, "max_orders"))      cfg.maxOrders = toUint("max_orders", *v);
    if (auto v = find(obj, "max_levels"))      cfg.maxLevels = toUint("max_levels", *v);
    if (auto v = find(obj, "retained_orders")) cfg.retainedOrders = toUint("retained_orders", *v);
    return cfg;
  }
  throw ParseError("unknown op '" + std::string(op) + "'");
}

inline Command parseCommand(std::string_view line) {
  boost::system::error_code ec;
  bj::value v = bj::parse(line, ec);
  if (ec) throw ParseError("invalid JSON: " + ec.message());
  if (!v.is_object()) throw ParseError("command must be a JSON object");
  return parseCommand(v.get_object());
}

// --- formatting ---------------------------------------------------------------

namespace detail {

  inline void putPrice(bj::object & obj, const Price & price) {
    obj["price"]    = price.value;
    obj["decimals"] = price.decimals;
  }

  inline bj::value level(const std::optional<LevelInfo> & lvl) {
    if (!lvl) return nullptr;
    bj::object obj;
    putPrice(obj, lvl->price);
    obj["qty"]    = lvl->qty;
    obj["orders"] = lvl->orderCount;
    return obj;
  }

  inline bj::array levels(const std::vector<LevelInfo> & lvls) {
    bj::array arr;
    for (const auto & l : lvls) arr.push_back(level(l));
    return arr;
  }

}

inline bj::object toJson(const Event & event) {
  bj::object obj;
  obj["seq"] = event.seqno;
  std::visit([&](const auto & e) {
    using T = std::decay_t<decltype(e)>;
    if constexpr (std::is_same_v<T, OrderAccepted>) {
      obj["event"] = "accepted";
      obj["id"]    = e.id;
    }
    else if constexpr (std::is_same_v<T, OrderRejected>) {
      obj["event"]  = "rejected";
      obj["id"]     = e.id;
      obj["reason"] = name(e.reason);
    }
    else if constexpr (std::is_same_v<T, Trade>) {
      obj["event"]         = "trade";
      obj["maker"]         = e.makerOrderId;
      obj["taker"]         = e.takerOrderId;
      obj["maker_account"] = e.makerAccount;
      obj["taker_account"] = e.takerAccount;
      obj["taker_side"]    = name(e.takerSide);
      detail::putPrice(obj, e.price);
      obj["qty"]             = e.qty;
      obj["maker_remaining"] = e.makerRemaining;
      obj["taker_remaining"] = e.takerRemaining;
    }
    else if constexpr (std::is_same_v<T, OrderCanceled>) {
      obj["event"]     = "canceled";
      obj["id"]        = e.id;
      obj["remaining"] = e.remaining;
      obj["reason"]    = name(e.reason);
    }
    else if constexpr (std::is_same_v<T, OrderRested>) {
      obj["event"] = "rested";
      obj["id"]    = e.id;
      detail::putPrice(obj, e.price);
      obj["remaining"] = e.remaining;
    }
    else if constexpr (std::is_same_v<T, CancelRejected>) {
      obj["event"]  = "cancel_rejected";
      obj["id"]     = e.id;
      obj["reason"] = name(e.reason);
    }
    else {
      static_assert(sizeof(T) == 0, "unhandled event type");
    }
  }, event.body);
  return obj;
}

// kind: "query" for explicit queries, "snapshot" for automatic snapshots
inline bj::object toJson(std::string_view kind, const TopOfBook & top) {
  bj::object obj;
  obj[kind]  = "top";
  obj["bid"] = detail::level(top.bid);
  obj["ask"] = detail::level(top.ask);
  return obj;
}

inline bj::object toJson(std::string_view kind, const BookSnapshot & book) {
  bj::object obj;
  obj[kind]   = "depth";
  obj["bids"] = detail::levels(book.bids);
  obj["asks"] = detail::levels(book.asks);
  return obj;
}

inline bj::object toJson(OrderId id, const std::optional<Order> & order) {
  bj::object obj;
  obj["query"] = "status";
  obj["id"]    = id;
  if (!order) {
    obj["status"] = nullptr;
    return obj;
  }
  obj["status"]  = name(order->status);
  obj["account"] = order->account;
  obj["side"]    = name(order->side);
  obj["type"]    = name(order->type);
  obj["tif"]     = name(order->tif);
  if (!order->isMarket()) detail::putPrice(obj, order->price);
  obj["size"]      = order->size;
  obj["filled"]    = order->filled;
  obj["remaining"] = order->isOpen() ? order->remaining() : 0;
  return obj;
}

inline bj::object errorJson(size_t line, std::string_view message) {
  bj::object obj;
  obj["error"] = message;
  obj["line"]  = line;
  return obj;
}

}
