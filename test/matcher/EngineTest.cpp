#include <boost/test/unit_test.hpp>
#include <map>
#include <hw/matcher/MatchingEngine.hpp>
#include "TestHelpers.hpp"

using namespace hw::matcher;
using namespace hw::matcher::test;

namespace {

constexpr auto BUY  = Side::Buy;
constexpr auto SELL = Side::Sell;
constexpr auto GTC  = TimeInForce::GTC;
constexpr auto IOC  = TimeInForce::IOC;

// checks one trade's essential fields
void checkTrade(const Trade & t, OrderId maker, OrderId taker, Price price, Quantity qty,
                Quantity makerRemaining, Quantity takerRemaining) {
  BOOST_CHECK_EQUAL(t.makerOrderId, maker);
  BOOST_CHECK_EQUAL(t.takerOrderId, taker);
  BOOST_CHECK_EQUAL(t.price, price);
  BOOST_CHECK_EQUAL(t.qty, qty);
  BOOST_CHECK_EQUAL(t.makerRemaining, makerRemaining);
  BOOST_CHECK_EQUAL(t.takerRemaining, takerRemaining);
}

// book must never be crossed after a request has been fully processed
void checkNotCrossed(const MatchingEngine & engine) {
  auto top = engine.top();
  if (top.bid && top.ask) BOOST_REQUIRE(top.bid->price < top.ask->price);
}

}

// ============================================================================ validation

BOOST_AUTO_TEST_SUITE(EngineValidationTests)

BOOST_FIXTURE_TEST_CASE(RejectsNonPositiveSize, EngineFixture) {
  for (Quantity size : {Quantity(0), Quantity(-5)}) {
    const OrderId id = static_cast<OrderId>(10 - size);
    auto & r = submit(limit(id, 1, BUY, 100, size));
    BOOST_REQUIRE_EQUAL(r.events.size(), 1u);
    BOOST_CHECK_EQUAL(r.at<OrderRejected>(0).reason, RejectReason::InvalidSize);
    BOOST_CHECK_EQUAL(engine.status(id)->status, OrderStatus::Rejected);
  }
  BOOST_CHECK_EQUAL(engine.book().orderCount(), 0u);
}

BOOST_FIXTURE_TEST_CASE(RejectsInvalidLimitPrice, EngineFixture) {
  BOOST_CHECK_EQUAL(submit(limit(1, 1, BUY, 0, 1)).at<OrderRejected>(0).reason, RejectReason::InvalidPrice);
  BOOST_CHECK_EQUAL(submit(limit(2, 1, BUY, -100, 1)).at<OrderRejected>(0).reason, RejectReason::InvalidPrice);
  BOOST_CHECK_EQUAL(submit(limit(3, 1, BUY, 100, 1, GTC, Price::MAX_DECIMALS + 1)).at<OrderRejected>(0).reason,
                    RejectReason::InvalidPrice);
  BOOST_CHECK(submit(limit(4, 1, BUY, 100, 1, GTC, Price::MAX_DECIMALS)).is<OrderAccepted>(0));
}

BOOST_FIXTURE_TEST_CASE(RejectsMarketGtc, EngineFixture) {
  auto & r = submit(market(1, 1, BUY, 5, GTC));
  BOOST_REQUIRE_EQUAL(r.events.size(), 1u);
  BOOST_CHECK_EQUAL(r.at<OrderRejected>(0).reason, RejectReason::InvalidTimeInForce);
}

BOOST_FIXTURE_TEST_CASE(RejectsDuplicateIdOfRestingOrder, EngineFixture) {
  submit(limit(1, 1, BUY, 100, 5));
  auto & r = submit(limit(1, 2, SELL, 200, 5));
  BOOST_REQUIRE_EQUAL(r.events.size(), 1u);
  BOOST_CHECK_EQUAL(r.at<OrderRejected>(0).reason, RejectReason::DuplicateOrderId);
  auto st = engine.status(1);                                 // original order untouched
  BOOST_CHECK_EQUAL(st->side, BUY);
  BOOST_CHECK_EQUAL(st->status, OrderStatus::New);
}

BOOST_FIXTURE_TEST_CASE(RejectsDuplicateIdOfFinishedOrder, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 5));
  submit(limit(2, 2, BUY, 100, 5));                           // #1 and #2 fully filled
  BOOST_CHECK_EQUAL(submit(limit(1, 3, BUY, 100, 1)).at<OrderRejected>(0).reason, RejectReason::DuplicateOrderId);
  BOOST_CHECK_EQUAL(submit(limit(2, 3, BUY, 100, 1)).at<OrderRejected>(0).reason, RejectReason::DuplicateOrderId);
  submit(limit(3, 1, BUY, 0, 1));                             // rejected id is also taken
  BOOST_CHECK_EQUAL(submit(limit(3, 1, BUY, 100, 1)).at<OrderRejected>(0).reason, RejectReason::DuplicateOrderId);
}

BOOST_FIXTURE_TEST_CASE(RejectedOrderDoesNotTouchBook, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 5));
  submit(limit(2, 2, BUY, 0, 5));
  BOOST_CHECK_EQUAL(engine.status(1)->remaining(), 5);
  BOOST_CHECK_EQUAL(engine.book().orderCount(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================ resting / basic matching

BOOST_AUTO_TEST_SUITE(EngineMatchingTests)

BOOST_FIXTURE_TEST_CASE(GtcRestsOnEmptyBook, EngineFixture) {
  auto & r = submit(limit(1, 1, BUY, 100, 5));
  BOOST_REQUIRE_EQUAL(r.events.size(), 2u);
  BOOST_CHECK_EQUAL(r.at<OrderAccepted>(0).id, 1u);
  BOOST_CHECK_EQUAL(r.at<OrderRested>(1).id, 1u);
  BOOST_CHECK_EQUAL(r.at<OrderRested>(1).remaining, 5);
  BOOST_CHECK_EQUAL(r.at<OrderRested>(1).price, Price(100));
  BOOST_CHECK_EQUAL(engine.top().bid->price, Price(100));
  BOOST_CHECK_EQUAL(engine.status(1)->status, OrderStatus::New);
}

BOOST_FIXTURE_TEST_CASE(NonCrossingOrdersBothRest, EngineFixture) {
  submit(limit(1, 1, BUY, 99, 5));
  auto & r = submit(limit(2, 2, SELL, 100, 5));
  BOOST_CHECK_EQUAL(r.count<Trade>(), 0u);
  BOOST_CHECK(r.is<OrderRested>(1));
  BOOST_CHECK_EQUAL(engine.top().bid->price, Price(99));
  BOOST_CHECK_EQUAL(engine.top().ask->price, Price(100));
}

BOOST_FIXTURE_TEST_CASE(ExactFullFill, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 5));
  auto & r = submit(limit(2, 2, BUY, 100, 5));
  BOOST_REQUIRE_EQUAL(r.events.size(), 2u);                   // accepted, trade; nothing rests
  BOOST_CHECK(r.is<OrderAccepted>(0));
  const Trade & t = r.at<Trade>(1);
  checkTrade(t, 1, 2, Price(100), 5, 0, 0);
  BOOST_CHECK_EQUAL(t.makerAccount, 1u);
  BOOST_CHECK_EQUAL(t.takerAccount, 2u);
  BOOST_CHECK_EQUAL(t.takerSide, BUY);
  BOOST_CHECK_EQUAL(engine.book().orderCount(), 0u);
  BOOST_CHECK_EQUAL(engine.status(1)->status, OrderStatus::Filled);
  BOOST_CHECK_EQUAL(engine.status(2)->status, OrderStatus::Filled);
}

BOOST_FIXTURE_TEST_CASE(TradeExecutesAtMakerPrice, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 5));
  auto & r = submit(limit(2, 2, BUY, 105, 5));                // aggressive buyer gets price improvement
  BOOST_CHECK_EQUAL(r.at<Trade>(1).price, Price(100));
  submit(limit(3, 1, BUY, 90, 5));
  auto & r2 = submit(limit(4, 2, SELL, 80, 5));
  BOOST_CHECK_EQUAL(r2.at<Trade>(1).price, Price(90));
}

BOOST_FIXTURE_TEST_CASE(PricePriorityBestPriceFirst, EngineFixture) {
  submit(limit(1, 1, SELL, 102, 1));
  submit(limit(2, 1, SELL, 100, 1));
  submit(limit(3, 1, SELL, 101, 1));
  auto & r = submit(limit(4, 2, BUY, 102, 3));
  auto trades = r.all<Trade>();
  BOOST_REQUIRE_EQUAL(trades.size(), 3u);
  BOOST_CHECK_EQUAL(trades[0].makerOrderId, 2u);
  BOOST_CHECK_EQUAL(trades[1].makerOrderId, 3u);
  BOOST_CHECK_EQUAL(trades[2].makerOrderId, 1u);
  BOOST_CHECK_EQUAL(trades[0].price, Price(100));
  BOOST_CHECK_EQUAL(trades[2].price, Price(102));
}

BOOST_FIXTURE_TEST_CASE(PricePriorityBidsHighestFirst, EngineFixture) {
  submit(limit(1, 1, BUY, 99, 1));
  submit(limit(2, 1, BUY, 101, 1));
  submit(limit(3, 1, BUY, 100, 1));
  auto trades = submit(limit(4, 2, SELL, 99, 3)).all<Trade>();
  BOOST_REQUIRE_EQUAL(trades.size(), 3u);
  BOOST_CHECK_EQUAL(trades[0].makerOrderId, 2u);
  BOOST_CHECK_EQUAL(trades[1].makerOrderId, 3u);
  BOOST_CHECK_EQUAL(trades[2].makerOrderId, 1u);
}

BOOST_FIXTURE_TEST_CASE(TimePriorityWithinLevel, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 2));
  submit(limit(2, 2, SELL, 100, 2));
  submit(limit(3, 3, SELL, 100, 2));
  auto trades = submit(limit(4, 4, BUY, 100, 5)).all<Trade>();
  BOOST_REQUIRE_EQUAL(trades.size(), 3u);
  checkTrade(trades[0], 1, 4, Price(100), 2, 0, 3);
  checkTrade(trades[1], 2, 4, Price(100), 2, 0, 1);
  checkTrade(trades[2], 3, 4, Price(100), 1, 1, 0);
  BOOST_CHECK_EQUAL(engine.status(3)->remaining(), 1);
}

BOOST_FIXTURE_TEST_CASE(PricePriorityBeatsTimePriority, EngineFixture) {
  submit(limit(1, 1, SELL, 101, 5));                          // earlier, worse price
  submit(limit(2, 2, SELL, 100, 5));                          // later, better price
  auto trades = submit(limit(3, 3, BUY, 101, 5)).all<Trade>();
  BOOST_REQUIRE_EQUAL(trades.size(), 1u);
  BOOST_CHECK_EQUAL(trades[0].makerOrderId, 2u);
}

BOOST_FIXTURE_TEST_CASE(SweepMultipleLevelsStopsAtLimit, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 2));
  submit(limit(2, 1, SELL, 101, 2));
  submit(limit(3, 1, SELL, 102, 2));
  auto & r = submit(limit(4, 2, BUY, 101, 10));
  auto trades = r.all<Trade>();
  BOOST_REQUIRE_EQUAL(trades.size(), 2u);                     // 102 is beyond the limit
  BOOST_REQUIRE(r.is<OrderRested>(r.events.size() - 1));
  BOOST_CHECK_EQUAL(r.at<OrderRested>(r.events.size() - 1).remaining, 6);
  BOOST_CHECK_EQUAL(engine.top().bid->price, Price(101));
  BOOST_CHECK_EQUAL(engine.top().ask->price, Price(102));
  checkNotCrossed(engine);
}

BOOST_FIXTURE_TEST_CASE(EventSequenceNumbersStrictlyIncrease, EngineFixture) {
  SeqNo last = 0;
  auto collect = [&](const Recorder & r) {
    for (auto & e : r.events) {
      BOOST_CHECK_EQUAL(e.seqno, last + 1);
      last = e.seqno;
    }
  };
  collect(submit(limit(1, 1, SELL, 100, 5)));
  collect(submit(limit(2, 2, BUY, 100, 2)));
  collect(submit(limit(3, 2, BUY, 0, 2)));
  collect(cancel(99));
  collect(cancel(1));
  BOOST_CHECK_EQUAL(engine.lastEventSeqNo(), last);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================ partial fills

BOOST_AUTO_TEST_SUITE(EnginePartialFillTests)

BOOST_FIXTURE_TEST_CASE(PartiallyFilledMakerKeepsPriority, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 10));
  submit(limit(2, 2, BUY, 100, 4));                           // #1 now 6 left
  submit(limit(3, 3, SELL, 100, 10));                         // later at same price
  auto trades = submit(limit(4, 4, BUY, 100, 7)).all<Trade>();
  BOOST_REQUIRE_EQUAL(trades.size(), 2u);
  checkTrade(trades[0], 1, 4, Price(100), 6, 0, 1);           // #1 still first
  checkTrade(trades[1], 3, 4, Price(100), 1, 9, 0);
}

BOOST_FIXTURE_TEST_CASE(PartiallyFilledTakerRestsRemainder, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 3));
  auto & r = submit(limit(2, 2, BUY, 100, 10));
  BOOST_REQUIRE_EQUAL(r.events.size(), 3u);                   // accepted, trade, rested
  checkTrade(r.at<Trade>(1), 1, 2, Price(100), 3, 0, 7);
  BOOST_CHECK_EQUAL(r.at<OrderRested>(2).remaining, 7);
  auto st = engine.status(2);
  BOOST_CHECK_EQUAL(st->status, OrderStatus::PartiallyFilled);
  BOOST_CHECK_EQUAL(st->filled, 3);
  BOOST_CHECK_EQUAL(engine.top().bid->qty, 7);
}

BOOST_FIXTURE_TEST_CASE(RestedRemainderIsBehindExistingOrdersAtLevel, EngineFixture) {
  submit(limit(1, 1, BUY, 100, 5));                           // resting bid
  submit(limit(2, 2, SELL, 101, 2));
  submit(limit(3, 3, BUY, 101, 5));                           // takes 2, rests 3 @101
  submit(limit(4, 4, BUY, 100, 5));                           // behind #1 at 100
  auto trades = submit(limit(5, 5, SELL, 100, 13)).all<Trade>();
  BOOST_REQUIRE_EQUAL(trades.size(), 3u);
  BOOST_CHECK_EQUAL(trades[0].makerOrderId, 3u);
  BOOST_CHECK_EQUAL(trades[1].makerOrderId, 1u);
  BOOST_CHECK_EQUAL(trades[2].makerOrderId, 4u);
}

BOOST_FIXTURE_TEST_CASE(MultiplePartialFillsAccumulate, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 10));
  for (OrderId id = 2; id <= 4; ++id) submit(limit(id, 2, BUY, 100, 3));
  auto st = engine.status(1);
  BOOST_CHECK_EQUAL(st->filled, 9);
  BOOST_CHECK_EQUAL(st->remaining(), 1);
  BOOST_CHECK_EQUAL(st->status, OrderStatus::PartiallyFilled);
  BOOST_CHECK_EQUAL(engine.top().ask->qty, 1);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================ IOC and market orders

BOOST_AUTO_TEST_SUITE(EngineIocMarketTests)

BOOST_FIXTURE_TEST_CASE(IocPartialFillCancelsRemainder, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 3));
  auto & r = submit(limit(2, 2, BUY, 100, 10, IOC));
  BOOST_REQUIRE_EQUAL(r.events.size(), 3u);
  checkTrade(r.at<Trade>(1), 1, 2, Price(100), 3, 0, 7);
  const auto & c = r.at<OrderCanceled>(2);
  BOOST_CHECK_EQUAL(c.id, 2u);
  BOOST_CHECK_EQUAL(c.remaining, 7);
  BOOST_CHECK_EQUAL(c.reason, CancelReason::IOCRemainder);
  BOOST_CHECK(!engine.top().bid);                             // IOC never rests
  auto st = engine.status(2);
  BOOST_CHECK_EQUAL(st->status, OrderStatus::Canceled);
  BOOST_CHECK_EQUAL(st->filled, 3);
}

BOOST_FIXTURE_TEST_CASE(IocNoLiquidityCancelsAll, EngineFixture) {
  submit(limit(1, 1, SELL, 101, 3));
  auto & r = submit(limit(2, 2, BUY, 100, 5, IOC));
  BOOST_REQUIRE_EQUAL(r.events.size(), 2u);
  BOOST_CHECK_EQUAL(r.at<OrderCanceled>(1).remaining, 5);
  BOOST_CHECK_EQUAL(engine.status(2)->filled, 0);
  BOOST_CHECK_EQUAL(engine.book().orderCount(), 1u);
}

BOOST_FIXTURE_TEST_CASE(IocFullyFilledHasNoCancel, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 5));
  auto & r = submit(limit(2, 2, BUY, 100, 5, IOC));
  BOOST_CHECK_EQUAL(r.count<OrderCanceled>(), 0u);
  BOOST_CHECK_EQUAL(engine.status(2)->status, OrderStatus::Filled);
}

BOOST_FIXTURE_TEST_CASE(IocRespectsLimitPrice, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 2));
  submit(limit(2, 1, SELL, 105, 2));
  auto & r = submit(limit(3, 2, BUY, 102, 4, IOC));
  BOOST_CHECK_EQUAL(r.count<Trade>(), 1u);
  BOOST_CHECK_EQUAL(r.all<OrderCanceled>().at(0).remaining, 2);
}

BOOST_FIXTURE_TEST_CASE(MarketSweepsAnyPrice, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 2));
  submit(limit(2, 1, SELL, 500, 2));
  submit(limit(3, 1, SELL, 99999, 2));
  auto & r = submit(market(4, 2, BUY, 6));
  auto trades = r.all<Trade>();
  BOOST_REQUIRE_EQUAL(trades.size(), 3u);
  BOOST_CHECK_EQUAL(trades[2].price, Price(99999));
  BOOST_CHECK_EQUAL(r.count<OrderCanceled>(), 0u);
  BOOST_CHECK(!engine.top().ask);
}

BOOST_FIXTURE_TEST_CASE(MarketRemainderCanceled, EngineFixture) {
  submit(limit(1, 1, BUY, 100, 2));
  auto & r = submit(market(2, 2, SELL, 5));
  BOOST_CHECK_EQUAL(r.count<Trade>(), 1u);
  const auto c = r.all<OrderCanceled>().at(0);
  BOOST_CHECK_EQUAL(c.remaining, 3);
  BOOST_CHECK_EQUAL(c.reason, CancelReason::IOCRemainder);
  BOOST_CHECK(!engine.top().ask);                             // market never rests
}

BOOST_FIXTURE_TEST_CASE(MarketOnEmptyBookCanceled, EngineFixture) {
  auto & r = submit(market(1, 1, BUY, 5));
  BOOST_REQUIRE_EQUAL(r.events.size(), 2u);
  BOOST_CHECK(r.is<OrderAccepted>(0));
  BOOST_CHECK_EQUAL(r.at<OrderCanceled>(1).remaining, 5);
  BOOST_CHECK_EQUAL(engine.status(1)->status, OrderStatus::Canceled);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================ self-trade prevention

BOOST_AUTO_TEST_SUITE(EngineSelfTradeTests)

BOOST_FIXTURE_TEST_CASE(CancelNewestCancelsIncoming, EngineFixture) {
  submit(limit(1, 7, SELL, 100, 5));
  auto & r = submit(limit(2, 7, BUY, 100, 5));
  BOOST_REQUIRE_EQUAL(r.events.size(), 2u);
  BOOST_CHECK_EQUAL(r.count<Trade>(), 0u);
  const auto & c = r.at<OrderCanceled>(1);
  BOOST_CHECK_EQUAL(c.id, 2u);
  BOOST_CHECK_EQUAL(c.remaining, 5);
  BOOST_CHECK_EQUAL(c.reason, CancelReason::SelfTrade);
  BOOST_CHECK_EQUAL(engine.status(1)->status, OrderStatus::New);   // resting untouched
  BOOST_CHECK_EQUAL(engine.status(2)->status, OrderStatus::Canceled);
  BOOST_CHECK(!engine.top().bid);                                 // GTC remainder does not rest
}

BOOST_FIXTURE_TEST_CASE(CancelNewestAfterPartialFillWithOthers, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 2));                          // other account, first in queue
  submit(limit(2, 7, SELL, 100, 5));                          // own order
  submit(limit(3, 1, SELL, 100, 5));                          // other account, behind own
  auto & r = submit(limit(4, 7, BUY, 100, 10));
  auto trades = r.all<Trade>();
  BOOST_REQUIRE_EQUAL(trades.size(), 1u);
  checkTrade(trades[0], 1, 4, Price(100), 2, 0, 8);
  const auto c = r.all<OrderCanceled>().at(0);
  BOOST_CHECK_EQUAL(c.id, 4u);
  BOOST_CHECK_EQUAL(c.remaining, 8);
  BOOST_CHECK_EQUAL(engine.status(4)->filled, 2);
  BOOST_CHECK_EQUAL(engine.status(3)->remaining(), 5);        // never reached
}

BOOST_AUTO_TEST_CASE(CancelOldestCancelsRestingAndContinues) {
  EngineFixture f(SelfTradePolicy::CancelOldest);
  f.submit(limit(1, 7, SELL, 100, 5));                        // own
  f.submit(limit(2, 1, SELL, 100, 5));                        // other
  auto & r = f.submit(limit(3, 7, BUY, 100, 4));
  BOOST_REQUIRE_EQUAL(r.events.size(), 3u);                   // accepted, canceled(#1), trade(#2)
  const auto & c = r.at<OrderCanceled>(1);
  BOOST_CHECK_EQUAL(c.id, 1u);
  BOOST_CHECK_EQUAL(c.remaining, 5);
  BOOST_CHECK_EQUAL(c.reason, CancelReason::SelfTrade);
  checkTrade(r.at<Trade>(2), 2, 3, Price(100), 4, 1, 0);
  BOOST_CHECK_EQUAL(f.engine.status(1)->status, OrderStatus::Canceled);
}

BOOST_AUTO_TEST_CASE(CancelOldestRemovesAllOwnOrdersThenRests) {
  EngineFixture f(SelfTradePolicy::CancelOldest);
  f.submit(limit(1, 7, SELL, 100, 1));
  f.submit(limit(2, 7, SELL, 101, 1));
  auto & r = f.submit(limit(3, 7, BUY, 101, 5));
  BOOST_CHECK_EQUAL(r.count<OrderCanceled>(), 2u);
  BOOST_CHECK_EQUAL(r.count<Trade>(), 0u);
  BOOST_CHECK_EQUAL(r.all<OrderRested>().at(0).remaining, 5);
  BOOST_CHECK(!f.engine.top().ask);
}

BOOST_AUTO_TEST_CASE(CancelBothCancelsBoth) {
  EngineFixture f(SelfTradePolicy::CancelBoth);
  f.submit(limit(1, 7, SELL, 100, 5));
  f.submit(limit(2, 1, SELL, 100, 5));
  auto & r = f.submit(limit(3, 7, BUY, 100, 5));
  BOOST_REQUIRE_EQUAL(r.events.size(), 3u);                   // accepted, canceled(#1), canceled(#3)
  BOOST_CHECK_EQUAL(r.at<OrderCanceled>(1).id, 1u);
  BOOST_CHECK_EQUAL(r.at<OrderCanceled>(2).id, 3u);
  BOOST_CHECK_EQUAL(r.at<OrderCanceled>(2).remaining, 5);
  BOOST_CHECK_EQUAL(f.engine.status(2)->remaining(), 5);      // other account untouched
}

BOOST_FIXTURE_TEST_CASE(SameAccountNonCrossingIsFine, EngineFixture) {
  submit(limit(1, 7, SELL, 101, 5));
  auto & r = submit(limit(2, 7, BUY, 100, 5));                // no match, no STP action
  BOOST_CHECK(r.is<OrderRested>(1));
  BOOST_CHECK_EQUAL(engine.book().orderCount(), 2u);
}

BOOST_FIXTURE_TEST_CASE(SelfTradeWithMarketOrder, EngineFixture) {
  submit(limit(1, 7, SELL, 100, 5));
  auto & r = submit(market(2, 7, BUY, 5));
  BOOST_CHECK_EQUAL(r.count<Trade>(), 0u);
  BOOST_CHECK_EQUAL(r.all<OrderCanceled>().at(0).reason, CancelReason::SelfTrade);
  BOOST_CHECK_EQUAL(r.count<OrderCanceled>(), 1u);            // not also an IOC remainder cancel
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================ cancel

BOOST_AUTO_TEST_SUITE(EngineCancelTests)

BOOST_FIXTURE_TEST_CASE(CancelRestingOrder, EngineFixture) {
  submit(limit(1, 1, BUY, 100, 5));
  auto & r = cancel(1);
  BOOST_REQUIRE_EQUAL(r.events.size(), 1u);
  const auto & c = r.at<OrderCanceled>(0);
  BOOST_CHECK_EQUAL(c.id, 1u);
  BOOST_CHECK_EQUAL(c.remaining, 5);
  BOOST_CHECK_EQUAL(c.reason, CancelReason::UserRequested);
  BOOST_CHECK(!engine.top().bid);
  BOOST_CHECK_EQUAL(engine.status(1)->status, OrderStatus::Canceled);
}

BOOST_FIXTURE_TEST_CASE(CancelUnknownOrder, EngineFixture) {
  auto & r = cancel(42);
  BOOST_REQUIRE_EQUAL(r.events.size(), 1u);
  BOOST_CHECK_EQUAL(r.at<CancelRejected>(0).id, 42u);
  BOOST_CHECK_EQUAL(r.at<CancelRejected>(0).reason, RejectReason::UnknownOrder);
  BOOST_CHECK(!engine.status(42));
}

BOOST_FIXTURE_TEST_CASE(CancelTwice, EngineFixture) {
  submit(limit(1, 1, BUY, 100, 5));
  cancel(1);
  BOOST_CHECK(cancel(1).is<CancelRejected>(0));
}

BOOST_FIXTURE_TEST_CASE(CancelPartiallyFilledReportsRemaining, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 10));
  submit(limit(2, 2, BUY, 100, 4));
  auto & r = cancel(1);
  BOOST_CHECK_EQUAL(r.at<OrderCanceled>(0).remaining, 6);
  auto st = engine.status(1);
  BOOST_CHECK_EQUAL(st->status, OrderStatus::Canceled);
  BOOST_CHECK_EQUAL(st->filled, 4);                           // fills preserved
  BOOST_CHECK(!engine.top().ask);
}

BOOST_FIXTURE_TEST_CASE(CancelKeepsOthersPriority, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 1));
  submit(limit(2, 2, SELL, 100, 1));
  submit(limit(3, 3, SELL, 100, 1));
  cancel(2);
  auto trades = submit(limit(4, 4, BUY, 100, 2)).all<Trade>();
  BOOST_REQUIRE_EQUAL(trades.size(), 2u);
  BOOST_CHECK_EQUAL(trades[0].makerOrderId, 1u);
  BOOST_CHECK_EQUAL(trades[1].makerOrderId, 3u);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================ cancel-during-match edge cases
// The engine is a single-threaded actor, so a cancel can never interleave with a match in
// progress: it is applied either entirely before or entirely after. These tests pin down
// the observable outcome for each ordering.

BOOST_AUTO_TEST_SUITE(EngineCancelDuringMatchTests)

BOOST_FIXTURE_TEST_CASE(CancelArrivesAfterMakerFullyFilled, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 5));
  submit(limit(2, 2, BUY, 100, 5));                           // fills #1 completely
  auto & r = cancel(1);                                       // too late
  BOOST_CHECK_EQUAL(r.at<CancelRejected>(0).reason, RejectReason::UnknownOrder);
  BOOST_CHECK_EQUAL(engine.status(1)->status, OrderStatus::Filled);
}

BOOST_FIXTURE_TEST_CASE(CancelArrivesBeforeMatch, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 5));
  submit(limit(2, 1, SELL, 101, 5));
  cancel(1);                                                  // best ask canceled first
  auto trades = submit(limit(3, 2, BUY, 101, 5)).all<Trade>();
  BOOST_REQUIRE_EQUAL(trades.size(), 1u);
  BOOST_CHECK_EQUAL(trades[0].makerOrderId, 2u);
  BOOST_CHECK_EQUAL(trades[0].price, Price(101));
}

BOOST_FIXTURE_TEST_CASE(CancelAfterPartialFillDuringSweep, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 10));
  submit(market(2, 2, BUY, 4));                               // partially consumes #1
  auto & r = cancel(1);
  BOOST_CHECK_EQUAL(r.at<OrderCanceled>(0).remaining, 6);
  BOOST_CHECK(submit(market(3, 2, BUY, 1)).is<OrderCanceled>(1));  // nothing left
}

BOOST_FIXTURE_TEST_CASE(CancelOfTakerThatNeverRested, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 3));
  submit(limit(2, 2, BUY, 100, 5, IOC));                      // IOC: remainder canceled
  BOOST_CHECK(cancel(2).is<CancelRejected>(0));
  submit(limit(3, 2, BUY, 100, 5));                           // no liquidity: rests
  BOOST_CHECK(cancel(3).is<OrderCanceled>(0));
}

BOOST_AUTO_TEST_CASE(CancelOfOrderRemovedBySelfTradePrevention) {
  EngineFixture f(SelfTradePolicy::CancelOldest);
  f.submit(limit(1, 7, SELL, 100, 5));
  f.submit(limit(2, 7, BUY, 100, 5));                         // STP cancels #1, #2 rests
  BOOST_CHECK(f.cancel(1).is<CancelRejected>(0));
  BOOST_CHECK(f.cancel(2).is<OrderCanceled>(0));
}

BOOST_FIXTURE_TEST_CASE(CancelRestedRemainderOfPartiallyFilledTaker, EngineFixture) {
  submit(limit(1, 1, SELL, 100, 3));
  submit(limit(2, 2, BUY, 100, 10));                          // 3 filled, 7 rest
  auto & r = cancel(2);
  BOOST_CHECK_EQUAL(r.at<OrderCanceled>(0).remaining, 7);
  BOOST_CHECK_EQUAL(engine.status(2)->filled, 3);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================ decimal prices

BOOST_AUTO_TEST_SUITE(EngineDecimalPriceTests)

BOOST_FIXTURE_TEST_CASE(SamePriceDifferentScaleShareLevelAndPriority, EngineFixture) {
  submit(limit(1, 1, SELL, 10010, 2, GTC, 2));                // 100.10
  submit(limit(2, 2, SELL, 1001, 2, GTC, 1));                 // 100.1: same level, behind #1
  BOOST_CHECK_EQUAL(engine.depth().asks.size(), 1u);
  BOOST_CHECK_EQUAL(engine.top().ask->qty, 4);
  auto trades = submit(limit(3, 3, BUY, 100100, 3, GTC, 3)).all<Trade>();   // 100.100
  BOOST_REQUIRE_EQUAL(trades.size(), 2u);
  BOOST_CHECK_EQUAL(trades[0].makerOrderId, 1u);
  BOOST_CHECK_EQUAL(trades[1].makerOrderId, 2u);
  BOOST_CHECK_EQUAL(trades[1].price.decimals, 1);             // maker's representation
}

BOOST_FIXTURE_TEST_CASE(CrossingDecidedByValueNotScale, EngineFixture) {
  submit(limit(1, 1, SELL, 10001, 5, GTC, 2));                // 100.01
  BOOST_CHECK_EQUAL(submit(limit(2, 2, BUY, 100, 5)).count<Trade>(), 0u);          // 100 < 100.01
  BOOST_CHECK_EQUAL(submit(limit(3, 2, BUY, 1000100, 5, GTC, 4)).count<Trade>(), 1u);  // 100.0100
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================ queries

BOOST_AUTO_TEST_SUITE(EngineQueryTests)

BOOST_FIXTURE_TEST_CASE(TopAndDepth, EngineFixture) {
  submit(limit(1, 1, BUY, 99, 1));
  submit(limit(2, 1, BUY, 100, 2));
  submit(limit(3, 1, BUY, 100, 3));
  submit(limit(4, 2, SELL, 102, 4));
  submit(limit(5, 2, SELL, 101, 5));
  auto top = engine.top();
  BOOST_CHECK_EQUAL(top.bid->price, Price(100));
  BOOST_CHECK_EQUAL(top.bid->qty, 5);
  BOOST_CHECK_EQUAL(top.bid->orderCount, 2u);
  BOOST_CHECK_EQUAL(top.ask->price, Price(101));
  auto d = engine.depth();
  BOOST_REQUIRE_EQUAL(d.bids.size(), 2u);
  BOOST_REQUIRE_EQUAL(d.asks.size(), 2u);
  BOOST_CHECK_EQUAL(d.bids[1].price, Price(99));
  BOOST_CHECK_EQUAL(d.asks[1].price, Price(102));
  BOOST_CHECK_EQUAL(engine.depth(1).bids.size(), 1u);
}

BOOST_FIXTURE_TEST_CASE(StatusLifecycle, EngineFixture) {
  BOOST_CHECK(!engine.status(1));
  submit(limit(1, 1, SELL, 100, 10));
  BOOST_CHECK_EQUAL(engine.status(1)->status, OrderStatus::New);
  submit(limit(2, 2, BUY, 100, 4));
  BOOST_CHECK_EQUAL(engine.status(1)->status, OrderStatus::PartiallyFilled);
  submit(limit(3, 2, BUY, 100, 6));
  auto st = engine.status(1);
  BOOST_CHECK_EQUAL(st->status, OrderStatus::Filled);
  BOOST_CHECK_EQUAL(st->filled, 10);
  BOOST_CHECK_EQUAL(st->account, 1u);
  BOOST_CHECK_EQUAL(st->side, SELL);
  BOOST_CHECK_EQUAL(st->price, Price(100));
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================ determinism and invariants

BOOST_AUTO_TEST_SUITE(EngineDeterminismTests)

BOOST_AUTO_TEST_CASE(SameInputSameOutput) {
  const auto requests = randomRequests(20260927, 20000);
  auto run = [&](std::vector<Event> & events) {
    MatchingEngine engine(EngineConfig{.symbol = "BTC", .selfTradePolicy = SelfTradePolicy::CancelOldest});
    for (const auto & req : requests) engine.process(req, [&](const Event & e) { events.push_back(e); });
    return engine.depth();
  };
  std::vector<Event> a, b;
  auto depthA = run(a);
  auto depthB = run(b);
  BOOST_REQUIRE_EQUAL(a.size(), b.size());
  BOOST_CHECK(a == b);
  BOOST_REQUIRE_EQUAL(depthA.bids.size(), depthB.bids.size());
  BOOST_REQUIRE_EQUAL(depthA.asks.size(), depthB.asks.size());
  for (size_t i = 0; i < depthA.bids.size(); ++i) {
    BOOST_CHECK_EQUAL(depthA.bids[i].price, depthB.bids[i].price);
    BOOST_CHECK_EQUAL(depthA.bids[i].qty, depthB.bids[i].qty);
  }
  BOOST_CHECK(a.size() > 20000u);                             // stream is non-trivial
  BOOST_CHECK(std::count_if(a.begin(), a.end(), [](const Event & e) { return std::holds_alternative<Trade>(e.body); }) > 1000);
}

// Randomized invariant checks for every self-trade policy:
//  - book never crossed after a request
//  - every order: filled == sum of its trade quantities, filled <= size
//  - no trade between the same account
//  - trade price is always the maker's resting price and respects the taker's limit
//  - per request: accepted order ends with exactly one terminal/rest event unless fully filled
BOOST_AUTO_TEST_CASE(RandomizedInvariants) {
  for (auto stp : {SelfTradePolicy::CancelNewest, SelfTradePolicy::CancelOldest, SelfTradePolicy::CancelBoth}) {
    BOOST_TEST_CONTEXT("stp=" << toString(stp)) {
      MatchingEngine engine(EngineConfig{.symbol = "BTC", .selfTradePolicy = stp});
      std::map<OrderId, Quantity> tradedQty;
      std::map<OrderId, NewOrder> accepted;

      for (const auto & req : randomRequests(7 + static_cast<uint32_t>(stp), 5000)) {
        Recorder rec;
        engine.process(req, rec);
        if (auto no = std::get_if<NewOrder>(&req); no && !rec.events.empty() && rec.is<OrderAccepted>(0)) {
          accepted.emplace(no->id, *no);
          size_t closing = rec.count<OrderRested>();
          for (auto & c : rec.all<OrderCanceled>()) closing += (c.id == no->id);
          BOOST_REQUIRE_LE(closing, 1u);
          const auto st = engine.status(no->id);
          BOOST_REQUIRE(st);
          if (closing == 0) BOOST_REQUIRE_EQUAL(st->status, OrderStatus::Filled);
        }
        for (const auto & t : rec.all<Trade>()) {
          BOOST_REQUIRE_NE(t.makerAccount, t.takerAccount);
          BOOST_REQUIRE_GT(t.qty, 0);
          tradedQty[t.makerOrderId] += t.qty;
          tradedQty[t.takerOrderId] += t.qty;
          const NewOrder & maker = accepted.at(t.makerOrderId);
          const NewOrder & taker = accepted.at(t.takerOrderId);
          BOOST_REQUIRE_EQUAL(t.price, maker.price);
          if (taker.type == OrderType::Limit) {
            BOOST_REQUIRE(taker.side == BUY ? t.price <= taker.price : t.price >= taker.price);
          }
        }
        checkNotCrossed(engine);
      }

      for (const auto & [id, order] : accepted) {
        const auto st = engine.status(id);
        BOOST_REQUIRE(st);
        BOOST_REQUIRE_EQUAL(st->filled, tradedQty[id]);
        BOOST_REQUIRE_LE(st->filled, st->size);
      }
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================ capacity and retention

BOOST_AUTO_TEST_SUITE(EngineCapacityTests)

BOOST_AUTO_TEST_CASE(OrderPoolFullOfRestingOrdersRejects) {
  MatchingEngine engine(EngineConfig{.symbol = "BTC", .maxOrders = 2, .maxLevels = 8});
  Recorder rec;
  engine.process(limit(1, 1, BUY, 100, 1), rec);
  engine.process(limit(2, 1, SELL, 110, 1), rec);
  rec.clear();
  engine.process(limit(3, 2, BUY, 99, 1), rec);
  BOOST_REQUIRE_EQUAL(rec.events.size(), 1u);
  BOOST_CHECK_EQUAL(rec.at<OrderRejected>(0).reason, RejectReason::CapacityExceeded);
  BOOST_CHECK(!engine.status(3));                              // not stored
  BOOST_CHECK_EQUAL(engine.book().orderCount(), 2u);

  rec.clear();
  engine.process(CancelOrder{1}, rec);                          // #1 retired, pool slot reusable
  engine.process(limit(3, 2, BUY, 99, 1), rec);
  BOOST_CHECK(rec.is<OrderAccepted>(1));
  BOOST_CHECK(!engine.status(1));                              // evicted to make room
  BOOST_CHECK(engine.status(3));
}

BOOST_AUTO_TEST_CASE(InvalidOrderOnFullPoolKeepsItsReason) {
  MatchingEngine engine(EngineConfig{.symbol = "BTC", .maxOrders = 1, .maxLevels = 8});
  Recorder rec;
  engine.process(limit(1, 1, BUY, 100, 1), rec);
  rec.clear();
  engine.process(limit(2, 1, BUY, 100, 0), rec);
  BOOST_CHECK_EQUAL(rec.at<OrderRejected>(0).reason, RejectReason::InvalidSize);
}

BOOST_AUTO_TEST_CASE(RetentionEvictsOldestFinished) {
  MatchingEngine engine(EngineConfig{.symbol = "BTC", .maxOrders = 16, .maxLevels = 8, .retainedOrders = 2});
  Recorder rec;
  for (OrderId id = 1; id <= 3; ++id) engine.process(limit(id, 1, BUY, 100, 1, IOC), rec);   // canceled: finished
  BOOST_CHECK_EQUAL(engine.retainedCount(), 2u);
  BOOST_CHECK(!engine.status(1));
  BOOST_CHECK_EQUAL(engine.status(2)->status, OrderStatus::Canceled);
  BOOST_CHECK_EQUAL(engine.status(3)->status, OrderStatus::Canceled);
  rec.clear();
  engine.process(limit(1, 1, BUY, 100, 1), rec);                // evicted id may be reused
  BOOST_CHECK(rec.is<OrderAccepted>(0));
  rec.clear();
  engine.process(limit(2, 1, BUY, 100, 1), rec);                // retained id still a duplicate
  BOOST_CHECK_EQUAL(rec.at<OrderRejected>(0).reason, RejectReason::DuplicateOrderId);
}

BOOST_AUTO_TEST_CASE(ZeroRetentionForgetsFinishedOrders) {
  MatchingEngine engine(EngineConfig{.symbol = "BTC", .maxOrders = 8, .maxLevels = 8, .retainedOrders = 0});
  Recorder rec;
  engine.process(limit(1, 1, SELL, 100, 5), rec);
  engine.process(limit(2, 2, BUY, 100, 3), rec);                // #2 filled, #1 partially
  BOOST_CHECK(!engine.status(2));
  BOOST_CHECK_EQUAL(engine.status(1)->filled, 3);               // resting orders always known
  BOOST_CHECK_EQUAL(engine.retainedCount(), 0u);
  BOOST_CHECK_EQUAL(engine.orderPool().inUse(), 1u);
  rec.clear();
  engine.process(CancelOrder{1}, rec);
  BOOST_CHECK_EQUAL(rec.at<OrderCanceled>(0).remaining, 2);    // event emitted before release
  BOOST_CHECK_EQUAL(rec.at<OrderCanceled>(0).id, 1u);
  BOOST_CHECK_EQUAL(engine.orderPool().inUse(), 0u);
}

BOOST_AUTO_TEST_CASE(LevelPoolFullCancelsRemainder) {
  MatchingEngine engine(EngineConfig{.symbol = "BTC", .maxOrders = 16, .maxLevels = 2});
  Recorder rec;
  engine.process(limit(1, 1, BUY, 100, 1), rec);
  engine.process(limit(2, 1, SELL, 110, 5), rec);
  rec.clear();
  engine.process(limit(3, 2, BUY, 101, 1), rec);                // would need a third level
  BOOST_REQUIRE_EQUAL(rec.events.size(), 2u);
  BOOST_CHECK(rec.is<OrderAccepted>(0));
  BOOST_CHECK_EQUAL(rec.at<OrderCanceled>(1).reason, CancelReason::CapacityExceeded);
  BOOST_CHECK_EQUAL(engine.status(3)->status, OrderStatus::Canceled);

  rec.clear();
  engine.process(limit(4, 2, BUY, 100, 1), rec);                // existing level: rests
  BOOST_CHECK(rec.is<OrderRested>(1));

  rec.clear();
  engine.process(limit(5, 2, BUY, 120, 8), rec);                // trades 5 @110 (frees level), rests 3 @120
  BOOST_REQUIRE_EQUAL(rec.events.size(), 3u);
  BOOST_CHECK(rec.is<Trade>(1));
  BOOST_CHECK_EQUAL(rec.at<OrderRested>(2).remaining, 3);
}

BOOST_AUTO_TEST_CASE(EngineDestructionWithLiveOrders) {
  {
    MatchingEngine engine(EngineConfig{.symbol = "BTC", .maxOrders = 64, .maxLevels = 16, .retainedOrders = 8});
    Recorder rec;
    for (const auto & req : randomRequests(5, 500)) engine.process(req, rec);
    BOOST_CHECK_GT(engine.book().orderCount(), 0u);
  }                                                              // asserts / ASan catch dangling links or leaks
  BOOST_CHECK(true);
}

// Small pools under random load: capacity limits are hit constantly; accounting must stay exact
BOOST_AUTO_TEST_CASE(RandomizedUnderCapacityPressure) {
  for (size_t retained : {size_t(0), size_t(8), std::numeric_limits<size_t>::max()}) {
    BOOST_TEST_CONTEXT("retained=" << retained) {
      MatchingEngine engine(EngineConfig{.symbol = "BTC", .maxOrders = 64, .maxLevels = 12, .retainedOrders = retained});
      size_t capacityRejects = 0, capacityCancels = 0;
      for (const auto & req : randomRequests(77, 20000)) {
        Recorder rec;
        engine.process(req, rec);
        for (auto & r : rec.all<OrderRejected>()) capacityRejects += r.reason == RejectReason::CapacityExceeded;
        for (auto & c : rec.all<OrderCanceled>()) capacityCancels += c.reason == CancelReason::CapacityExceeded;
        checkNotCrossed(engine);
        BOOST_REQUIRE_EQUAL(engine.orderPool().inUse(), engine.book().orderCount() + engine.retainedCount());
        BOOST_REQUIRE_LE(engine.orderPool().inUse(), 64u);
        BOOST_REQUIRE_LE(engine.retainedCount(), retained);
        BOOST_REQUIRE_LE(engine.book().levelCount(BUY) + engine.book().levelCount(SELL), 12u);
      }
      BOOST_CHECK_GT(capacityCancels, 0u);                      // level limit was exercised
      if (retained == 0) BOOST_CHECK_GT(capacityRejects, 0u);   // order limit was exercised
      BOOST_CHECK_EQUAL(engine.orderPool().fallbackCount(), 0u);
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()
