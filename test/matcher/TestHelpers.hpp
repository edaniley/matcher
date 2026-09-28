#pragma once
// Shared helpers for matcher unit tests
#include <ostream>
#include <random>
#include <vector>
#include <hw/matcher/MatchingEngine.hpp>

namespace hw::matcher {

// Boost.Test prints values of failed BOOST_CHECK_EQUAL; give it readable output
inline std::ostream & operator << (std::ostream & os, const Price & p) { return os << p.toString(); }
inline std::ostream & operator << (std::ostream & os, Side v)          { return os << toString(v); }
inline std::ostream & operator << (std::ostream & os, OrderStatus v)   { return os << toString(v); }
inline std::ostream & operator << (std::ostream & os, RejectReason v)  { return os << toString(v); }
inline std::ostream & operator << (std::ostream & os, CancelReason v)  { return os << toString(v); }

}

namespace hw::matcher::test {

inline NewOrder limit(OrderId id, AccountId account, Side side, int64_t price, Quantity size,
                      TimeInForce tif = TimeInForce::GTC, uint8_t decimals = 0) {
  return NewOrder{.id = id, .account = account, .side = side, .type = OrderType::Limit,
                  .tif = tif, .price = Price(price, decimals), .size = size};
}

inline NewOrder market(OrderId id, AccountId account, Side side, Quantity size,
                       TimeInForce tif = TimeInForce::IOC) {
  return NewOrder{.id = id, .account = account, .side = side, .type = OrderType::Market,
                  .tif = tif, .price = Price{}, .size = size};
}

// Event sink recording everything the engine emits
struct Recorder {
  std::vector<Event> events;

  void operator () (const Event & e) { events.push_back(e); }

  template <typename T>
  std::vector<T> all() const {
    std::vector<T> out;
    for (const auto & e : events) {
      if (auto p = std::get_if<T>(&e.body)) out.push_back(*p);
    }
    return out;
  }

  template <typename T>
  size_t count() const { return all<T>().size(); }

  template <typename T>
  const T & at(size_t i) const { return std::get<T>(events.at(i).body); }

  template <typename T>
  bool is(size_t i) const { return std::holds_alternative<T>(events.at(i).body); }

  void clear() { events.clear(); }
};

// Engine + recorder; submit() clears the recorder so each step's events can be inspected
struct EngineFixture {
  MatchingEngine engine;
  Recorder       rec;

  explicit EngineFixture(SelfTradePolicy stp = SelfTradePolicy::CancelNewest)
    : engine(EngineConfig{.symbol = "BTC", .selfTradePolicy = stp}) {}

  const Recorder & submit(const Request & req) {
    rec.clear();
    engine.process(req, rec);
    return rec;
  }
  const Recorder & cancel(OrderId id) { return submit(CancelOrder{id}); }
};

// Deterministic random request stream (fixed seed); ids are unique except where
// duplicates are injected on purpose.
inline std::vector<Request> randomRequests(uint32_t seed, size_t count) {
  std::mt19937_64 rng(seed);
  auto pick = [&](int64_t lo, int64_t hi) { return std::uniform_int_distribution<int64_t>(lo, hi)(rng); };
  std::vector<Request> out;
  OrderId nextId = 1;
  for (size_t i = 0; i < count; ++i) {
    const int64_t kind = pick(0, 99);
    if (kind < 20 && nextId > 1) {
      out.push_back(CancelOrder{static_cast<OrderId>(pick(1, static_cast<int64_t>(nextId)))});
      continue;
    }
    NewOrder o;
    o.id      = (kind == 20 && nextId > 1) ? static_cast<OrderId>(pick(1, static_cast<int64_t>(nextId - 1))) : nextId++;
    o.account = static_cast<AccountId>(pick(1, 5));
    o.side    = pick(0, 1) ? Side::Buy : Side::Sell;
    o.size    = pick(1, 20);
    if (kind < 30) {
      o.type = OrderType::Market;
      o.tif  = TimeInForce::IOC;
    }
    else {
      o.type = OrderType::Limit;
      o.tif  = kind < 45 ? TimeInForce::IOC : TimeInForce::GTC;
      // same price in two scales: 95.0 .. 105.0 as {950..1050, 1} or {9500..10500, 2}
      const int64_t ticks = pick(950, 1050);
      o.price = pick(0, 1) ? Price(ticks, 1) : Price(ticks * 10, 2);
    }
    out.push_back(o);
  }
  return out;
}

}
