#include <boost/test/unit_test.hpp>
#include <boost/json/src.hpp>       // Boost.JSON header-only: compiled once in this translation unit
#include <sstream>
#include "Replay.hpp"
#include "TestHelpers.hpp"

using namespace hw::matcher;
using namespace hw::matcher::json;

namespace {

// runs a replay over 'input' and returns output lines
std::vector<std::string> replay(const std::string & input, ReplayOptions options = {}) {
  std::istringstream in(input);
  std::ostringstream out;
  Replay r(std::move(options), out);
  r.run(in);
  std::vector<std::string> lines;
  std::istringstream res(out.str());
  for (std::string line; std::getline(res, line); ) lines.push_back(line);
  return lines;
}

std::string replayText(const std::string & input, ReplayOptions options = {}) {
  std::istringstream in(input);
  std::ostringstream out;
  Replay(std::move(options), out).run(in);
  return out.str();
}

std::string errorOf(const std::string & line) {
  try {
    parseCommand(line);
  }
  catch (const ParseError & e) {
    return e.what();
  }
  return {};
}

}

BOOST_AUTO_TEST_SUITE(JsonParseTests)

BOOST_AUTO_TEST_CASE(ParseFullLimitOrder) {
  auto cmd = parseCommand(R"({"op":"new","id":1,"account":7,"side":"buy","type":"limit","tif":"ioc","price":6543210,"decimals":2,"size":5})");
  const auto & o = std::get<NewOrder>(cmd);
  BOOST_CHECK_EQUAL(o.id, 1u);
  BOOST_CHECK_EQUAL(o.account, 7u);
  BOOST_CHECK_EQUAL(o.side, Side::Buy);
  BOOST_CHECK(o.type == OrderType::Limit);
  BOOST_CHECK(o.tif == TimeInForce::IOC);
  BOOST_CHECK_EQUAL(o.price.value, 6543210);
  BOOST_CHECK_EQUAL(o.price.decimals, 2);
  BOOST_CHECK_EQUAL(o.size, 5);
}

BOOST_AUTO_TEST_CASE(ParseDefaults) {
  const auto o = std::get<NewOrder>(parseCommand(R"({"op":"new","id":1,"account":1,"side":"SELL","price":100,"size":1})"));
  BOOST_CHECK(o.type == OrderType::Limit);
  BOOST_CHECK(o.tif == TimeInForce::GTC);
  BOOST_CHECK_EQUAL(o.price.decimals, 0);
  BOOST_CHECK_EQUAL(o.side, Side::Sell);                      // enum names are case-insensitive
}

BOOST_AUTO_TEST_CASE(ParseMarketWithoutPrice) {
  const auto o = std::get<NewOrder>(parseCommand(R"({"op":"new","id":1,"account":1,"side":"buy","type":"market","tif":"ioc","size":3})"));
  BOOST_CHECK(o.type == OrderType::Market);
  BOOST_CHECK_EQUAL(o.price, Price());
}

BOOST_AUTO_TEST_CASE(ParseOtherCommands) {
  BOOST_CHECK_EQUAL(std::get<CancelOrder>(parseCommand(R"({"op":"cancel","id":9})")).id, 9u);
  BOOST_CHECK(std::holds_alternative<TopQuery>(parseCommand(R"({"op":"top"})")));
  BOOST_CHECK_EQUAL(std::get<DepthQuery>(parseCommand(R"({"op":"depth","levels":3})")).levels, 3u);
  BOOST_CHECK_EQUAL(std::get<DepthQuery>(parseCommand(R"({"op":"depth"})")).levels, std::numeric_limits<size_t>::max());
  BOOST_CHECK_EQUAL(std::get<StatusQuery>(parseCommand(R"({"op":"status","id":4})")).id, 4u);
  const auto cfg = std::get<ConfigCmd>(parseCommand(R"({"op":"config","symbol":"ETH","stp":"cancel_both","snapshot":"depth"})"));
  BOOST_CHECK_EQUAL(*cfg.symbol, "ETH");
  BOOST_CHECK(*cfg.selfTradePolicy == SelfTradePolicy::CancelBoth);
  BOOST_CHECK(*cfg.snapshot == SnapshotMode::Depth);
  BOOST_CHECK(!cfg.maxOrders && !cfg.maxLevels && !cfg.retainedOrders);
  const auto cap = std::get<ConfigCmd>(parseCommand(R"({"op":"config","max_orders":10,"max_levels":4,"retained_orders":0})"));
  BOOST_CHECK_EQUAL(*cap.maxOrders, 10u);
  BOOST_CHECK_EQUAL(*cap.maxLevels, 4u);
  BOOST_CHECK_EQUAL(*cap.retainedOrders, 0u);
}

BOOST_AUTO_TEST_CASE(ParseErrors) {
  BOOST_CHECK_NE(errorOf("not json").find("invalid JSON"), std::string::npos);
  BOOST_CHECK_NE(errorOf("[1,2]").find("must be a JSON object"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"id":1})").find("missing field 'op'"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"op":"fly"})").find("unknown op"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"op":"new","id":1,"account":1,"side":"buy","size":1})").find("'price'"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"op":"new","id":1,"account":1,"side":"up","price":1,"size":1})").find("invalid value 'up'"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"op":"new","id":1,"account":1,"side":"buy","price":1,"size":1,"colour":"red"})").find("unknown field 'colour'"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"op":"new","id":-1,"account":1,"side":"buy","price":1,"size":1})").find("non-negative"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"op":"new","id":1,"account":1,"side":"buy","price":1.5,"size":1})").find("must be an integer"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"op":"new","id":1,"account":1,"side":"buy","price":1,"decimals":300,"size":1})").find("out of range"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"op":"new","id":1,"account":1,"side":"buy","type":"market","decimals":2,"size":1})").find("without 'price'"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"op":"cancel"})").find("missing field 'id'"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"op":"top","x":1})").find("unknown field"), std::string::npos);
  BOOST_CHECK_NE(errorOf(R"({"op":"config","stp":"never"})").find("invalid value"), std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(JsonFormatTests)

BOOST_AUTO_TEST_CASE(EventFormats) {
  auto s = [](EventBody body) { return bj::serialize(toJson(Event{3, body})); };
  BOOST_CHECK_EQUAL(s(OrderAccepted{1}), R"({"seq":3,"event":"accepted","id":1})");
  BOOST_CHECK_EQUAL(s(OrderRejected{1, RejectReason::InvalidSize}), R"({"seq":3,"event":"rejected","id":1,"reason":"invalid_size"})");
  BOOST_CHECK_EQUAL(s(CancelRejected{1, RejectReason::UnknownOrder}), R"({"seq":3,"event":"cancel_rejected","id":1,"reason":"unknown_order"})");
  BOOST_CHECK_EQUAL(s(OrderCanceled{1, 4, CancelReason::IOCRemainder}), R"({"seq":3,"event":"canceled","id":1,"remaining":4,"reason":"ioc_remainder"})");
  BOOST_CHECK_EQUAL(s(OrderRested{1, Price(10010, 2), 4}), R"({"seq":3,"event":"rested","id":1,"price":10010,"decimals":2,"remaining":4})");
  BOOST_CHECK_EQUAL(s(Trade{1, 2, 10, 20, Side::Sell, Price(5, 1), 3, 7, 0}),
    R"({"seq":3,"event":"trade","maker":1,"taker":2,"maker_account":10,"taker_account":20,"taker_side":"sell","price":5,"decimals":1,"qty":3,"maker_remaining":7,"taker_remaining":0})");
}

BOOST_AUTO_TEST_CASE(QueryFormats) {
  TopOfBook top{LevelInfo{Price(100), 5, 2}, std::nullopt};
  BOOST_CHECK_EQUAL(bj::serialize(toJson("query", top)),
    R"({"query":"top","bid":{"price":100,"decimals":0,"qty":5,"orders":2},"ask":null})");
  BookSnapshot book{{LevelInfo{Price(100), 5, 2}}, {}};
  BOOST_CHECK_EQUAL(bj::serialize(toJson("snapshot", book)),
    R"({"snapshot":"depth","bids":[{"price":100,"decimals":0,"qty":5,"orders":2}],"asks":[]})");
  BOOST_CHECK_EQUAL(bj::serialize(toJson(OrderId(5), std::optional<Order>{})), R"({"query":"status","id":5,"status":null})");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ReplayTests)

BOOST_AUTO_TEST_CASE(SkipsBlankAndCommentLines) {
  auto out = replay("\n# comment\n   \n  # indented comment\n{\"op\":\"top\"}\n");
  BOOST_REQUIRE_EQUAL(out.size(), 1u);
  BOOST_CHECK_EQUAL(out[0], R"({"query":"top","bid":null,"ask":null})");
}

BOOST_AUTO_TEST_CASE(ErrorsReportLineAndContinue) {
  auto out = replay("{\"op\":\"top\"}\nbroken\n{\"op\":\"cancel\",\"id\":1}\n");
  BOOST_REQUIRE_EQUAL(out.size(), 3u);
  BOOST_CHECK_NE(out[1].find(R"("line":2)"), std::string::npos);
  BOOST_CHECK_EQUAL(out[2], R"({"seq":1,"event":"cancel_rejected","id":1,"reason":"unknown_order"})");
}

BOOST_AUTO_TEST_CASE(ConfigAppliesOnlyBeforeFirstCommand) {
  const std::string input =
    R"({"op":"config","stp":"cancel_oldest"})" "\n"
    R"({"op":"new","id":1,"account":7,"side":"sell","price":100,"size":5})" "\n"
    R"({"op":"new","id":2,"account":7,"side":"buy","price":100,"size":5})" "\n"
    R"({"op":"config","stp":"cancel_newest"})" "\n";
  auto out = replay(input);
  BOOST_REQUIRE_EQUAL(out.size(), 6u);
  BOOST_CHECK_EQUAL(out[3], R"({"seq":4,"event":"canceled","id":1,"remaining":5,"reason":"self_trade"})");   // oldest canceled
  BOOST_CHECK_EQUAL(out[4], R"({"seq":5,"event":"rested","id":2,"price":100,"decimals":0,"remaining":5})");
  BOOST_CHECK_NE(out[5].find("config must precede"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(SnapshotAfterEveryRequest) {
  ReplayOptions opts;
  opts.snapshot = SnapshotMode::Top;
  auto out = replay(R"({"op":"new","id":1,"account":1,"side":"buy","price":100,"size":5})" "\n"
                    R"({"op":"cancel","id":1})" "\n", opts);
  BOOST_REQUIRE_EQUAL(out.size(), 5u);
  BOOST_CHECK_EQUAL(out[2], R"({"snapshot":"top","bid":{"price":100,"decimals":0,"qty":5,"orders":1},"ask":null})");
  BOOST_CHECK_EQUAL(out[4], R"({"snapshot":"top","bid":null,"ask":null})");
}

BOOST_AUTO_TEST_CASE(StatusQueryOutput) {
  auto out = replay(R"({"op":"new","id":1,"account":3,"side":"sell","price":10010,"decimals":2,"size":5})" "\n"
                    R"({"op":"new","id":2,"account":4,"side":"buy","type":"market","tif":"ioc","size":2})" "\n"
                    R"({"op":"status","id":1})" "\n"
                    R"({"op":"status","id":2})" "\n"
                    R"({"op":"status","id":3})" "\n");
  BOOST_REQUIRE_EQUAL(out.size(), 7u);
  BOOST_CHECK_EQUAL(out[4], R"({"query":"status","id":1,"status":"partially_filled","account":3,"side":"sell","type":"limit","tif":"gtc","price":10010,"decimals":2,"size":5,"filled":2,"remaining":3})");
  BOOST_CHECK_EQUAL(out[5], R"({"query":"status","id":2,"status":"filled","account":4,"side":"buy","type":"market","tif":"ioc","size":2,"filled":2,"remaining":0})");
  BOOST_CHECK_EQUAL(out[6], R"({"query":"status","id":3,"status":null})");
}

// Generated JSON stream replayed twice must give byte-identical output
BOOST_AUTO_TEST_CASE(ReplayIsDeterministic) {
  std::ostringstream input;
  for (const auto & req : test::randomRequests(99, 5000)) {
    if (auto o = std::get_if<NewOrder>(&req)) {
      input << R"({"op":"new","id":)" << o->id << R"(,"account":)" << o->account
            << R"(,"side":")" << name(o->side) << R"(","type":")" << name(o->type)
            << R"(","tif":")" << name(o->tif) << '"';
      if (o->type == OrderType::Limit) input << R"(,"price":)" << o->price.value << R"(,"decimals":)" << int(o->price.decimals);
      input << R"(,"size":)" << o->size << "}\n";
    }
    else {
      input << R"({"op":"cancel","id":)" << std::get<CancelOrder>(req).id << "}\n";
    }
    input << R"({"op":"top"})" "\n";
  }
  ReplayOptions opts;
  opts.snapshot = SnapshotMode::Depth;
  const std::string a = replayText(input.str(), opts);
  const std::string b = replayText(input.str(), opts);
  BOOST_CHECK(a.size() > 100000u);
  BOOST_CHECK(a == b);
  BOOST_CHECK_EQUAL(a.find("\"error\""), std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
