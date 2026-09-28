#pragma once

#include <string_view>
#include <hw/type/NamedType.hpp>

namespace hw::type {

// FNV-1a 64-bit hash algorithm
constexpr size_t fnvla_hash(std::string_view str) {
  size_t hash = 0xcbf29ce484222325;
  for (char chr : str) {
    hash ^= static_cast<size_t>(chr);
    hash *= 0x100000001b3;
  }
  return hash;
}

template <typename Type>
struct TypeInfo {
  static constexpr std::string_view name() {
    std::string_view rawname = __PRETTY_FUNCTION__;

    #ifdef __clang__
      std::string_view prefix = "Type = ";
      std::string_view suffix = "]";
    #else // GCC
      std::string_view prefix = "with Type = ";
      std::string_view suffix = ";"; // GCC often uses ';' before additional template info
      if (rawname.find(prefix) == std::string_view::npos) {
          prefix = "[with T = "; // Fallback for some GCC versions
          suffix = "]";
      }
    #endif

    size_t start = rawname.find(prefix) + prefix.size();
    size_t end = rawname.find(suffix, start);
    if (end == std::string_view::npos) {
      end = rawname.rfind(']');
    }

    std::string_view result = rawname.substr(start, end - start);
    while (!result.empty() && result.back() == ' ') {
      result.remove_suffix(1);
    }

    return result;
  }

  static constexpr size_t name_hash = fnvla_hash(name());
};


template <typename Type>
constexpr std::string_view TypeName() {
  if constexpr (requires { { Type::name_tag } -> std::convertible_to<std::string_view>; }) {
    return static_cast<std::string_view>(Type::name_tag);
  }
  else if constexpr (requires { { Type::name_tag() } -> std::convertible_to<std::string_view>; }) {
    return Type::name_tag();
  }
  else {
    return TypeInfo<Type>::name();
  }
}

static_assert(TypeName<int>() == "int");
static_assert(TypeName<NamedType<"Bid", int>>() == "Bid");
static_assert(TypeName<NamedType<"Bid", int>>() != TypeName<NamedType<"Ask", int>>());


template <typename, typename = void>
struct HasValueType : std::false_type {};

template <typename Type>
struct HasValueType<Type, std::void_t<typename Type::value_type>> : std::true_type {};

template <typename Type, bool Condition = HasValueType<Type>::value>
struct UnderlyingValueType {
  using type = Type;
};

template <typename Type> struct UnderlyingValueType<Type, true> {
  using type = Type::value_type;
};

}
