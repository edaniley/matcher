#pragma once
#include <cstddef>
#include <type_traits>
#include <variant>
#include <tuple>
#include <array>
#include <memory>
#include <sstream>
#include <boost/mp11/list.hpp>
#include <boost/mp11/set.hpp>
// #include <boost/mp11/tuple.hpp>
#include <boost/mp11/algorithm.hpp>
#include <boost/mp11/function.hpp>
#include <hw/type/TypeInfo.hpp>

namespace hw::type {

using namespace boost::mp11;
template<typename... Args>
struct type_list: boost::mp11::mp_list<Args...> {
  using variant_type = std::variant<Args...>;
  using tuple_type   = std::tuple<Args...>;
  static const size_t SIZE = std::max({sizeof(Args)...});
};

template <typename TypeList>
constexpr uint64_t TypeListSignature() {
  uint64_t retval = 0xcbf29ce484222325;
  mp_for_each<mp_iota_c<mp_size<TypeList>::value>>([&retval] (auto I) {
    using Type = mp_at_c<TypeList, I>;
    retval ^= TypeInfo<Type>::name_hash ^ (sizeof(Type) << 1);
    retval *= 0x100000001b3;
  });
  return retval;
}

static_assert(TypeListSignature<type_list<int, double, char[4]>>() == TypeListSignature<type_list<int, double, char[4]>>());
static_assert(TypeListSignature<type_list<int, double, char[4]>>() != TypeListSignature<type_list<int, double, char[5]>>());
static_assert(TypeListSignature<type_list<int, double, char[4]>>() != TypeListSignature<type_list<double, int, char[4]>>());


template <typename T>
struct make_unique_ptr {
  using type = std:: unique_ptr<T>;
};

template <typename T>
using make_unique_ptr_t = make_unique_ptr<T>::type;

template < typename T>
struct make_shared_ptr {
  using type = std::shared_ptr<T>;
};

template <typename T>
using make_shared_ptr_t = make_shared_ptr<T>::type;

// template<typename Tuple, size_t I = 0>
// void tuple_for_each(auto && callee, Tuple & tuple) {
//   if constexpr (I < std::tuple_size_v<Tuple>) {
//     callee(std:: get<I>(tuple));
//     tuple_for_each<Tuple, I+1>(callee, tuple);
//   }
// }

template <typename TypeList>
constexpr size_t MaxTypeNameSize() {
  static_assert(mp_is_list<TypeList>::value);
  if constexpr (mp_size<TypeList>::value) {
    return std::max(
      std::size(TypeName<mp_at_c<TypeList, 0>>()),
      MaxTypeNameSize<mp_pop_front<TypeList>>()
    );
  }
  return 0;
}

template <typename TypeList>
constexpr size_t FindTypeByName(std::string_view name) {
  constexpr size_t N = mp_size<TypeList>::value;
  size_t result = N;
  mp_for_each<mp_iota_c<N>>([&](auto I) {
    if (result == N) {
      using T = mp_at_c<TypeList, I>;
      if (TypeName<T>() == name) {
        result = I;
      }
    }
  });

  return result;
}

static_assert(0 == FindTypeByName<type_list<int, double, char>>("int"));
static_assert(1 == FindTypeByName<type_list<int, double, char>>("double"));
static_assert(2 == FindTypeByName<type_list<int, double, char>>("char"));
static_assert(3 == FindTypeByName<type_list<int, double, char>>("?????"));

template <typename TypeList>
constexpr size_t TypeListDataSize() {
  if constexpr (mp_empty<TypeList>::value) {
    return 0;
  } else {
    return sizeof(mp_front<TypeList>) + TypeListDataSize<mp_pop_front<TypeList>>();
  }
}

static_assert(15 == TypeListDataSize<type_list<int, double, char, short>>());
static_assert(0 == TypeListDataSize<type_list<>>());

template <typename TypeList>
std::string TypeListToString() {
  std::ostringstream oss;
  oss << '[';
  mp_for_each<mp_iota_c<mp_size<TypeList>::value>> ([&] (auto I) {
    using FieldType = mp_at_c<TypeList, I>;
    // std:: cout << "\n" << TypeName<FieldType>() << std::endl;
    oss << ' ' << TypeName<FieldType>();
  });
  oss << " ]";
  return oss.str();
}

}

