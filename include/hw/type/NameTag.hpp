
#pragma once

#include <type_traits>
#include <string_view>
#include <compare>
#include <ostream>

namespace hw::type {

template <size_t N>
struct NameTag {

  static constexpr size_t tag_size = N - 1;

  constexpr NameTag(const char (&str)[N]) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
      _nametag[i] = str[i];
    }
  }

  constexpr std::string_view toString() const noexcept {
    // Exclude null terminator if present at the end
    return (N > 0 && _nametag[N - 1] == '\0')
        ? std::string_view(_nametag, N - 1)
        : std::string_view(_nametag, N);
  }

 constexpr explicit operator std::string_view() const noexcept {
    return toString();
  }

  // C++20 Spaceship operator using the string_view for lexicographical comparison
  constexpr auto operator <=> (const NameTag& other) const noexcept {
    return toString() <=> other.toString();
  }

  constexpr bool operator == (const NameTag& other) const noexcept {
    return toString() == other.toString();
  }

  char _nametag[N]{};
};

// Deduction guide (C++17) to allow NameTag("ping") without explicit size.
template <size_t N>
NameTag(const char (&)[N]) -> NameTag<N>;

template <size_t N>
std::ostream& operator << (std::ostream& os, const NameTag<N>& tag) {
  os << tag.toString ();
  return os;
}

static_assert (NameTag("ping") == NameTag("ping"));
static_assert (NameTag("ping") <  NameTag("pong"));
static_assert (NameTag("pong") >  NameTag("ping"));

}

