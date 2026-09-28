#pragma once
#include <array>
#include <compare>
#include <cstdint>
#include <string>

namespace hw::matcher {

//
// Fixed-point decimal price: value * 10^-decimals, e.g. {6543210, 2} == 65432.10
// - value and decimals travel together; neither is meaningful alone.
// - Comparison is by numeric value, independent of scale: {1001, 1} == {10010, 2}.
//   Hence the same price in different scales maps to the same book level.
// - Integer only; exact and deterministic. Scaling uses __int128, so it cannot
//   overflow for decimals <= MAX_DECIMALS.
//
struct Price {
  static constexpr uint8_t MAX_DECIMALS = 18;

  int64_t value    = 0;
  uint8_t decimals = 0;

  constexpr Price() = default;
  constexpr explicit Price(int64_t v, uint8_t d = 0) noexcept : value(v), decimals(d) {}

  constexpr bool isValid()    const noexcept { return decimals <= MAX_DECIMALS; }
  constexpr bool isPositive() const noexcept { return value > 0; }

  // same price expressed with 'targetDecimals'; false if that would lose precision or overflow
  constexpr bool rescale(uint8_t targetDecimals, Price & out) const noexcept {
    if (!isValid() || targetDecimals > MAX_DECIMALS) return false;
    if (targetDecimals >= decimals) {
      __int128 v = static_cast<__int128>(value) * pow10(targetDecimals - decimals);
      if (v > INT64_MAX || v < INT64_MIN) return false;
      out = Price(static_cast<int64_t>(v), targetDecimals);
      return true;
    }
    const __int128 div = pow10(decimals - targetDecimals);
    if (value % div != 0) return false;
    out = Price(static_cast<int64_t>(value / div), targetDecimals);
    return true;
  }

  // canonical form: trailing zeros stripped from value
  constexpr Price normalized() const noexcept {
    Price p = *this;
    while (p.decimals > 0 && p.value % 10 == 0) {
      p.value /= 10;
      --p.decimals;
    }
    return p;
  }

  friend constexpr std::strong_ordering operator <=> (const Price & a, const Price & b) noexcept {
    const uint8_t d = a.decimals > b.decimals ? a.decimals : b.decimals;
    const __int128 av = static_cast<__int128>(a.value) * pow10(d - a.decimals);
    const __int128 bv = static_cast<__int128>(b.value) * pow10(d - b.decimals);
    return av <=> bv;
  }

  friend constexpr bool operator == (const Price & a, const Price & b) noexcept {
    return (a <=> b) == 0;
  }

  // "65432.10"; keeps the given scale (trailing zeros are significant for display)
  std::string toString() const {
    const uint64_t magnitude = value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    std::string digits = std::to_string(magnitude);
    if (decimals > 0) {
      if (digits.size() <= decimals) digits.insert(0, decimals + 1 - digits.size(), '0');
      digits.insert(digits.size() - decimals, 1, '.');
    }
    return value < 0 ? "-" + digits : digits;
  }

private:
  static constexpr __int128 pow10(int n) noexcept {
    constexpr auto table = [] {
      std::array<__int128, MAX_DECIMALS + 1> t{};
      t[0] = 1;
      for (size_t i = 1; i < t.size(); ++i) t[i] = t[i - 1] * 10;
      return t;
    }();
    return table[n];
  }
};

}
