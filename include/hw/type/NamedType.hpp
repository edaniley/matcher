#pragma once

#include <string_view>
#include <hw/type/NameTag.hpp>

namespace hw::type {

// template <typename Type>
// concept NamedType = requires {
//   Type::name_tag();
// };

template <NameTag Tag, typename Type>
struct NamedType {
  static constexpr std::string_view name_tag () {
    return Tag.toString();
  }
  static constexpr size_t size() {
    return sizeof (Type);
  }

  operator const Type & () const  { return static_cast<const Type &> (*this); }
  operator Type &       ()        { return static_cast<Type &>(*this); }
};

static_assert(std::is_same_v< NamedType<"Bid", int>, NamedType<"Bid", int>>);
static_assert(!std::is_same_v<NamedType<"Bid", int>, NamedType<"Ask", int>>);
static_assert(!std::is_same_v<NamedType<"Bid", int>, NamedType<"Bid", short>>);

}
