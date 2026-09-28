#include <boost/test/unit_test.hpp>
#include <hw/matcher/Price.hpp>
#include "TestHelpers.hpp"

using hw::matcher::Price;

// compile-time checks
static_assert(Price(1001, 1) == Price(10010, 2));
static_assert(Price(1001, 1) <  Price(10011, 2));
static_assert(Price(100, 0)  >  Price(99999, 3));
static_assert(Price(-5, 0)   <  Price(1, 18));
static_assert(Price() == Price(0, 7));

BOOST_AUTO_TEST_SUITE(PriceTests)

BOOST_AUTO_TEST_CASE(DefaultIsZero) {
  Price p;
  BOOST_CHECK_EQUAL(p.value, 0);
  BOOST_CHECK_EQUAL(p.decimals, 0);
  BOOST_CHECK(p.isValid());
  BOOST_CHECK(!p.isPositive());
}

BOOST_AUTO_TEST_CASE(Validity) {
  BOOST_CHECK(Price(1, 0).isValid());
  BOOST_CHECK(Price(1, Price::MAX_DECIMALS).isValid());
  BOOST_CHECK(!Price(1, Price::MAX_DECIMALS + 1).isValid());
  BOOST_CHECK(Price(1, 2).isPositive());
  BOOST_CHECK(!Price(-1, 2).isPositive());
}

BOOST_AUTO_TEST_CASE(EqualityIgnoresScale) {
  BOOST_CHECK_EQUAL(Price(1001, 1), Price(10010, 2));
  BOOST_CHECK_EQUAL(Price(5, 0), Price(5000000, 6));
  BOOST_CHECK_NE(Price(1001, 1), Price(1001, 2));
  BOOST_CHECK_EQUAL(Price(0, 0), Price(0, 18));
}

BOOST_AUTO_TEST_CASE(OrderingAcrossScales) {
  BOOST_CHECK(Price(99, 0) < Price(9901, 2));
  BOOST_CHECK(Price(99011, 3) > Price(9901, 2));
  BOOST_CHECK(Price(-1, 0) < Price(-9, 1));
  BOOST_CHECK(Price(-1001, 1) == Price(-10010, 2));
  BOOST_CHECK((Price(3, 1) <=> Price(30, 2)) == std::strong_ordering::equal);
}

BOOST_AUTO_TEST_CASE(ExtremesDoNotOverflow) {
  const Price big(INT64_MAX, 0);
  const Price tiny(1, Price::MAX_DECIMALS);
  BOOST_CHECK(big > tiny);
  BOOST_CHECK(Price(INT64_MIN, 0) < Price(INT64_MIN, 18));
  BOOST_CHECK(Price(INT64_MAX, 18) < Price(10, 0));        // ~9.22 < 10
}

BOOST_AUTO_TEST_CASE(Normalized) {
  Price n = Price(1234000, 4).normalized();
  BOOST_CHECK_EQUAL(n.value, 1234);
  BOOST_CHECK_EQUAL(n.decimals, 1);
  n = Price(500, 2).normalized();
  BOOST_CHECK_EQUAL(n.value, 5);
  BOOST_CHECK_EQUAL(n.decimals, 0);
  n = Price(500, 0).normalized();                            // no decimals to strip
  BOOST_CHECK_EQUAL(n.value, 500);
  BOOST_CHECK_EQUAL(n.decimals, 0);
  n = Price(0, 5).normalized();
  BOOST_CHECK_EQUAL(n.value, 0);
  BOOST_CHECK_EQUAL(n.decimals, 0);
}

BOOST_AUTO_TEST_CASE(RescaleUpAndDown) {
  Price out;
  BOOST_REQUIRE(Price(1001, 1).rescale(3, out));
  BOOST_CHECK_EQUAL(out.value, 100100);
  BOOST_CHECK_EQUAL(out.decimals, 3);
  BOOST_REQUIRE(Price(100100, 3).rescale(1, out));
  BOOST_CHECK_EQUAL(out.value, 1001);
  BOOST_CHECK_EQUAL(out.decimals, 1);
  BOOST_REQUIRE(Price(7, 2).rescale(2, out));                // same scale
  BOOST_CHECK_EQUAL(out.value, 7);
}

BOOST_AUTO_TEST_CASE(RescaleFailures) {
  Price out(42, 0);
  BOOST_CHECK(!Price(1001, 1).rescale(0, out));              // loses precision
  BOOST_CHECK(!Price(INT64_MAX, 0).rescale(1, out));         // overflow
  BOOST_CHECK(!Price(INT64_MIN, 0).rescale(1, out));         // negative overflow
  BOOST_CHECK(!Price(1, 0).rescale(Price::MAX_DECIMALS + 1, out));
  BOOST_CHECK(!Price(1, Price::MAX_DECIMALS + 1).rescale(0, out));
  BOOST_CHECK_EQUAL(out.value, 42);                          // untouched on failure
}

BOOST_AUTO_TEST_CASE(ToString) {
  BOOST_CHECK_EQUAL(Price(6543210, 2).toString(), "65432.10");
  BOOST_CHECK_EQUAL(Price(5, 3).toString(), "0.005");
  BOOST_CHECK_EQUAL(Price(-5, 3).toString(), "-0.005");
  BOOST_CHECK_EQUAL(Price(42, 0).toString(), "42");
  BOOST_CHECK_EQUAL(Price(100, 2).toString(), "1.00");
  BOOST_CHECK_EQUAL(Price(0, 2).toString(), "0.00");
  BOOST_CHECK_EQUAL(Price(INT64_MIN, 0).toString(), "-9223372036854775808");
  BOOST_CHECK_EQUAL(Price(INT64_MAX, 18).toString(), "9.223372036854775807");
}

BOOST_AUTO_TEST_SUITE_END()
