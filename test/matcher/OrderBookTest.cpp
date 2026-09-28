#include <boost/test/unit_test.hpp>
#include <deque>
#include <map>
#include <random>
#include <hw/matcher/OrderBook.hpp>
#include "TestHelpers.hpp"

using namespace hw::matcher;

namespace {

Order makeOrder(OrderId id, Side side, Price price, Quantity size, AccountId account = 1) {
  Order o;
  o.id = id;
  o.account = account;
  o.side = side;
  o.price = price;
  o.size = size;
  o.seqno = id;
  return o;
}

// Orders are owned by the test (stable addresses); declared before the book so the book is
// destroyed first and unlinks them.
struct BookFixture {
  std::deque<Order> orders;
  OrderBook         book;

  explicit BookFixture(size_t maxLevels = 64) : book("BTC", maxLevels) {}

  Order & order(OrderId id, Side side, Price price, Quantity size, AccountId account = 1) {
    return orders.emplace_back(makeOrder(id, side, price, size, account));
  }
  Order & add(OrderId id, Side side, Price price, Quantity size) {
    Order & o = order(id, side, price, size);
    BOOST_REQUIRE(book.add(o));
    return o;
  }
};

}

BOOST_AUTO_TEST_SUITE(OrderTests)

BOOST_AUTO_TEST_CASE(FillUpdatesStatus) {
  Order o = makeOrder(1, Side::Buy, Price(100), 10);
  BOOST_CHECK(o.isOpen());
  BOOST_CHECK(!o.isResting());
  BOOST_CHECK_EQUAL(o.remaining(), 10);
  o.fill(4);
  BOOST_CHECK_EQUAL(o.status, OrderStatus::PartiallyFilled);
  BOOST_CHECK_EQUAL(o.remaining(), 6);
  o.fill(6);
  BOOST_CHECK_EQUAL(o.status, OrderStatus::Filled);
  BOOST_CHECK(!o.isOpen());
}

BOOST_AUTO_TEST_CASE(Crosses) {
  Order buy = makeOrder(1, Side::Buy, Price(100), 1);
  BOOST_CHECK(buy.crosses(Price(99)));
  BOOST_CHECK(buy.crosses(Price(100)));
  BOOST_CHECK(buy.crosses(Price(10000, 2)));              // 100.00 == 100
  BOOST_CHECK(!buy.crosses(Price(10001, 2)));
  Order sell = makeOrder(2, Side::Sell, Price(100), 1);
  BOOST_CHECK(sell.crosses(Price(101)));
  BOOST_CHECK(sell.crosses(Price(100)));
  BOOST_CHECK(!sell.crosses(Price(9999, 2)));
  Order mkt = makeOrder(3, Side::Buy, Price(), 1);
  mkt.type = OrderType::Market;
  BOOST_CHECK(mkt.crosses(Price(1000000)));              // market crosses any price
}

BOOST_AUTO_TEST_CASE(Cancel) {
  Order o = makeOrder(1, Side::Sell, Price(100), 5);
  o.fill(2);
  o.cancel();
  BOOST_CHECK_EQUAL(o.status, OrderStatus::Canceled);
  BOOST_CHECK(!o.isOpen());
  BOOST_CHECK_EQUAL(o.filled, 2);
}

BOOST_AUTO_TEST_CASE(EqualityIgnoresListMembership) {
  Order a = makeOrder(1, Side::Buy, Price(100), 5);
  PriceLevel level(Price(100));
  level.append(a);
  Order copy = a;                                        // copy: unlinked hook, level pointer copied
  BOOST_CHECK(!copy.hook.isLinked());
  BOOST_CHECK(copy == a);
  copy.filled = 1;
  BOOST_CHECK(!(copy == a));
  level.erase(a);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(PriceLevelTests)

BOOST_AUTO_TEST_CASE(FifoAndTotals) {
  Order a = makeOrder(1, Side::Buy, Price(100), 5);
  Order b = makeOrder(2, Side::Buy, Price(100), 3);
  Order c = makeOrder(3, Side::Buy, Price(10000, 2), 2);  // same price, other scale
  PriceLevel level(Price(100));
  BOOST_CHECK(level.empty());
  level.append(a);
  level.append(b);
  level.append(c);
  BOOST_CHECK_EQUAL(level.orderCount(), 3u);
  BOOST_CHECK_EQUAL(level.totalQty(), 10);
  BOOST_CHECK_EQUAL(&level.front(), &a);
  BOOST_CHECK_EQUAL(a.level, &level);

  level.fill(a, 2);                                       // partial: keeps priority
  BOOST_CHECK_EQUAL(level.totalQty(), 8);
  BOOST_CHECK_EQUAL(&level.front(), &a);
  BOOST_CHECK_EQUAL(a.remaining(), 3);

  level.erase(a);
  BOOST_CHECK(!a.isResting());
  BOOST_CHECK(!a.hook.isLinked());
  BOOST_CHECK_EQUAL(a.filled, 2);
  BOOST_CHECK_EQUAL(level.totalQty(), 5);                 // only remaining qty was subtracted
  BOOST_CHECK_EQUAL(&level.front(), &b);

  std::vector<OrderId> ids;
  for (const Order & o : level) ids.push_back(o.id);
  BOOST_CHECK(ids == (std::vector<OrderId>{2, 3}));
  level.erase(b);
  level.erase(c);
  BOOST_CHECK(level.empty());
  BOOST_CHECK_EQUAL(level.totalQty(), 0);
}

BOOST_AUTO_TEST_CASE(DestructorUnlinksOrders) {
  Order a = makeOrder(1, Side::Sell, Price(7), 1);
  {
    PriceLevel level(Price(7));
    level.append(a);
  }
  BOOST_CHECK(!a.hook.isLinked());
  BOOST_CHECK(!a.isResting());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(OrderBookTests)

BOOST_FIXTURE_TEST_CASE(EmptyBook, BookFixture) {
  BOOST_CHECK_EQUAL(book.symbol(), "BTC");
  BOOST_CHECK_EQUAL(book.orderCount(), 0u);
  BOOST_CHECK(book.empty(Side::Buy) && book.empty(Side::Sell));
  BOOST_CHECK(!book.bestOrder(Side::Buy));
  BOOST_CHECK(!book.bestPrice(Side::Sell));
  BOOST_CHECK(!book.top().bid && !book.top().ask);
  BOOST_CHECK(book.depth().bids.empty() && book.depth().asks.empty());
  BOOST_CHECK_EQUAL(book.levelCapacity(), 64u);
  BOOST_CHECK_EQUAL(book.freeLevels(), 64u);
}

BOOST_FIXTURE_TEST_CASE(BidsSortedDescendingAsksAscending, BookFixture) {
  // inserted out of order: exercises insert at top, bottom and middle of the level vectors
  add(1, Side::Buy, Price(99), 1);
  add(2, Side::Buy, Price(101), 2);
  add(3, Side::Buy, Price(100), 3);
  add(4, Side::Buy, Price(98), 4);
  add(5, Side::Sell, Price(105), 5);
  add(6, Side::Sell, Price(103), 6);
  add(7, Side::Sell, Price(104), 7);
  add(8, Side::Sell, Price(106), 8);
  auto d = book.depth();
  BOOST_REQUIRE_EQUAL(d.bids.size(), 4u);
  BOOST_REQUIRE_EQUAL(d.asks.size(), 4u);
  const int64_t bids[] = {101, 100, 99, 98};
  const int64_t asks[] = {103, 104, 105, 106};
  for (size_t i = 0; i < 4; ++i) {
    BOOST_CHECK_EQUAL(d.bids[i].price, Price(bids[i]));
    BOOST_CHECK_EQUAL(d.asks[i].price, Price(asks[i]));
  }
  BOOST_CHECK_EQUAL(*book.bestPrice(Side::Buy), Price(101));
  BOOST_CHECK_EQUAL(*book.bestPrice(Side::Sell), Price(103));
  BOOST_CHECK_EQUAL(book.bestOrder(Side::Buy)->id, 2u);
  BOOST_CHECK_EQUAL(book.bestOrder(Side::Sell)->id, 6u);
  BOOST_CHECK_EQUAL(book.levelCount(Side::Buy), 4u);
  BOOST_CHECK_EQUAL(book.freeLevels(), 56u);

  auto limited = book.depth(2);
  BOOST_CHECK_EQUAL(limited.bids.size(), 2u);
  BOOST_CHECK_EQUAL(limited.asks.size(), 2u);
  BOOST_CHECK(book.depth(0).bids.empty());
}

BOOST_FIXTURE_TEST_CASE(LevelAggregation, BookFixture) {
  add(1, Side::Buy, Price(1001, 1), 5);
  add(2, Side::Buy, Price(10010, 2), 3);                  // same level as #1
  auto top = book.top();
  BOOST_REQUIRE(top.bid);
  BOOST_CHECK_EQUAL(top.bid->qty, 8);
  BOOST_CHECK_EQUAL(top.bid->orderCount, 2u);
  BOOST_CHECK(!top.ask);
  BOOST_CHECK_EQUAL(book.depth().bids.size(), 1u);
  BOOST_CHECK_EQUAL(book.freeLevels(), 63u);
}

BOOST_FIXTURE_TEST_CASE(FillPartialThenFull, BookFixture) {
  Order & a = add(1, Side::Sell, Price(100), 5);
  Order & b = add(2, Side::Sell, Price(100), 5);
  add(3, Side::Sell, Price(101), 5);
  book.fill(a, 2);
  BOOST_CHECK_EQUAL(book.bestOrder(Side::Sell), &a);     // partial fill keeps priority
  BOOST_CHECK_EQUAL(a.remaining(), 3);
  BOOST_CHECK_EQUAL(book.top().ask->qty, 8);
  book.fill(a, 3);                                        // fully filled: leaves the book
  BOOST_CHECK(!a.isResting());
  BOOST_CHECK_EQUAL(a.status, OrderStatus::Filled);
  BOOST_CHECK_EQUAL(book.bestOrder(Side::Sell), &b);
  book.fill(b, 5);                                        // level 100 emptied and released
  BOOST_CHECK_EQUAL(*book.bestPrice(Side::Sell), Price(101));
  BOOST_CHECK_EQUAL(book.orderCount(), 1u);
  BOOST_CHECK_EQUAL(book.levelCount(Side::Sell), 1u);
  BOOST_CHECK_EQUAL(book.freeLevels(), 63u);
}

BOOST_FIXTURE_TEST_CASE(RemoveFromMiddleAndLastOfLevel, BookFixture) {
  Order & a = add(1, Side::Buy, Price(100), 1);
  Order & b = add(2, Side::Buy, Price(100), 2);
  Order & c = add(3, Side::Buy, Price(100), 3);
  add(4, Side::Buy, Price(99), 4);
  add(5, Side::Buy, Price(101), 5);
  book.remove(b);
  BOOST_CHECK(!b.isResting());
  BOOST_CHECK_EQUAL(book.depth().bids[1].qty, 4);
  BOOST_CHECK_EQUAL(book.depth().bids[1].orderCount, 2u);
  book.remove(a);
  book.remove(c);                                         // middle level (100) emptied
  auto d = book.depth();
  BOOST_REQUIRE_EQUAL(d.bids.size(), 2u);
  BOOST_CHECK_EQUAL(d.bids[0].price, Price(101));
  BOOST_CHECK_EQUAL(d.bids[1].price, Price(99));
  BOOST_CHECK_EQUAL(book.orderCount(), 2u);
}

BOOST_AUTO_TEST_CASE(LevelPoolExhaustion) {
  BookFixture f(2);
  BOOST_CHECK(f.book.add(f.order(1, Side::Buy, Price(100), 1)));
  BOOST_CHECK(f.book.add(f.order(2, Side::Sell, Price(110), 1)));
  Order & third = f.order(3, Side::Buy, Price(99), 1);
  BOOST_CHECK(!f.book.add(third));                        // needs a third level: refused
  BOOST_CHECK(!third.isResting());
  BOOST_CHECK_EQUAL(f.book.orderCount(), 2u);
  BOOST_CHECK(f.book.add(f.order(4, Side::Buy, Price(100), 1)));   // existing level: fine
  f.book.remove(f.orders[1]);                             // frees the 110 level
  BOOST_CHECK(f.book.add(third));
  BOOST_CHECK_EQUAL(f.book.levelCount(Side::Buy), 2u);
}

BOOST_FIXTURE_TEST_CASE(ForEachOrderInPriorityOrder, BookFixture) {
  add(1, Side::Sell, Price(102), 1);
  add(2, Side::Sell, Price(101), 1);
  add(3, Side::Sell, Price(102), 1);
  add(4, Side::Sell, Price(101), 1);
  std::vector<OrderId> ids;
  book.forEachOrder(Side::Sell, [&](const Order & o) { ids.push_back(o.id); });
  BOOST_CHECK(ids == (std::vector<OrderId>{2, 4, 1, 3}));
}

BOOST_FIXTURE_TEST_CASE(ClearPassesEveryOrder, BookFixture) {
  add(1, Side::Buy, Price(100), 1);
  add(2, Side::Buy, Price(99), 1);
  add(3, Side::Sell, Price(101), 1);
  std::vector<OrderId> ids;
  book.clear([&](Order & o) { BOOST_CHECK(!o.isResting()); ids.push_back(o.id); });
  BOOST_CHECK_EQUAL(ids.size(), 3u);
  BOOST_CHECK_EQUAL(book.orderCount(), 0u);
  BOOST_CHECK(book.empty(Side::Buy) && book.empty(Side::Sell));
  BOOST_CHECK_EQUAL(book.freeLevels(), 64u);
}

// Random add / remove / fill against a std::map model of aggregated levels
BOOST_AUTO_TEST_CASE(RandomizedAgainstModel) {
  BookFixture f(256);
  std::map<int64_t, Quantity, std::greater<>> bidModel;   // price -> total qty
  std::map<int64_t, Quantity> askModel;
  std::vector<Order *> resting;
  std::mt19937_64 rng(11);
  OrderId nextId = 1;

  auto adjust = [&](Side s, int64_t px, Quantity q) {
    if (s == Side::Buy) { if ((bidModel[px] += q) == 0) bidModel.erase(px); }
    else                { if ((askModel[px] += q) == 0) askModel.erase(px); }
  };

  for (int op = 0; op < 20000; ++op) {
    const int kind = static_cast<int>(rng() % 3);
    if (kind == 0 || resting.empty()) {
      const Side side = rng() % 2 ? Side::Buy : Side::Sell;
      const int64_t px = side == Side::Buy ? 900 + static_cast<int64_t>(rng() % 100) : 1000 + static_cast<int64_t>(rng() % 100);
      const Quantity qty = 1 + static_cast<Quantity>(rng() % 10);
      Order & o = f.order(nextId++, side, Price(px), qty);
      BOOST_REQUIRE(f.book.add(o));
      resting.push_back(&o);
      adjust(side, px, qty);
    }
    else {
      const size_t idx = rng() % resting.size();
      Order & o = *resting[idx];
      const int64_t px = o.price.value;
      if (kind == 1) {
        adjust(o.side, px, -o.remaining());
        f.book.remove(o);
      }
      else {
        // fill only the best order of a side (as matching does)
        Order & best = *f.book.bestOrder(o.side);
        const Quantity q = 1 + static_cast<Quantity>(rng() % best.remaining());
        adjust(best.side, best.price.value, -q);
        f.book.fill(best, q);
        if (best.remaining() != 0) continue;
        resting.erase(std::find(resting.begin(), resting.end(), &best));
        continue;
      }
      resting[idx] = resting.back();
      resting.pop_back();
    }

    if (op % 64 == 0) {
      auto d = f.book.depth();
      BOOST_REQUIRE_EQUAL(d.bids.size(), bidModel.size());
      BOOST_REQUIRE_EQUAL(d.asks.size(), askModel.size());
      size_t i = 0;
      for (auto & [px, q] : bidModel) {
        BOOST_REQUIRE_EQUAL(d.bids[i].price, Price(px));
        BOOST_REQUIRE_EQUAL(d.bids[i].qty, q);
        ++i;
      }
      i = 0;
      for (auto & [px, q] : askModel) {
        BOOST_REQUIRE_EQUAL(d.asks[i].price, Price(px));
        BOOST_REQUIRE_EQUAL(d.asks[i].qty, q);
        ++i;
      }
      BOOST_REQUIRE_EQUAL(f.book.orderCount(), resting.size());
      BOOST_REQUIRE_EQUAL(f.book.freeLevels(), 256u - bidModel.size() - askModel.size());
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()
