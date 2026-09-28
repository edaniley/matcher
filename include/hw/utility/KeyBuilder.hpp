#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <tuple>
#include <algorithm>
#include <string_view>
#include <type_traits>
#include <initializer_list>

#include <hw/type/NameTag.hpp>
#include <hw/type/TypeList.hpp>

namespace hw::utility {

// Field Definition Tuple
// Usage: KeyAttribute<"Name", Size, Inserter>
template <type::NameTag Name, size_t Size, typename Inserter>
struct KeyAttribute {
  static constexpr auto name = Name;
  static constexpr size_t size = Size;
  using inserter_type = Inserter;
};

template <typename T, typename AccessorList>
class KeyBuilder;

// Specialization to unpack the AccessorList
// We specialize on type::type_list
template <typename T, typename... Fields>
class KeyBuilder<T, type::type_list<Fields...>> {

  // Helper to find a field by name in the AccessorList
  template <type::NameTag Name, typename Tuple>
  struct find_field;

  template <type::NameTag Name>
  struct find_field<Name, std::tuple<>> {
    using type = void; // Not found
  };

  template <type::NameTag Name, typename Head, typename... Tail>
  struct find_field<Name, std::tuple<Head, Tail...>> {
    using type = std::conditional_t<
      (Head::name.toString() == Name.toString()),
      Head,
      typename find_field<Name, std::tuple<Tail...>>::type
    >;
  };

  // Helper to parse comma separated list
  static constexpr std::string_view trim(std::string_view s) {
    size_t first = s.find_first_not_of(" \t");
    if (std::string_view::npos == first) {
      return s;
    }
    size_t last = s.find_last_not_of(" \t");
    return s.substr(first, (last - first + 1));
  }

  // Helper to detect default_byte member
  template <typename Attr, typename = void>
  struct get_default_byte {
    static constexpr std::byte value{0};
  };

  template <typename Attr>
  struct get_default_byte<Attr, std::void_t<decltype(Attr::default_byte)>> {
    static constexpr std::byte value = Attr::default_byte;
  };

  // Shared implementation for matchList
  template <type::NameTag... Tags>
  static constexpr bool matchTags(std::string_view list) noexcept {
    std::string_view remaining = list;
    size_t found_count = 0;

    while (!remaining.empty()) {
      size_t pos = remaining.find(',');
      std::string_view token = (pos == std::string_view::npos)
        ? remaining
        : remaining.substr(0, pos);

      token = trim(token);

      if (!token.empty()) {
        bool found = ((token == Tags.toString()) || ...);
        if (!found) return false;
        found_count++;
      }

      if (pos == std::string_view::npos) break;
      remaining.remove_prefix(pos + 1);
    }
    return found_count == sizeof...(Tags);
  }

public:
  // -------------------------------------------------------------------------
  // Builder: Constructs key using ONLY selected fields, in specified order.
  // -------------------------------------------------------------------------
  template <type::NameTag... SelectedFields>
  class Builder {
    using AllFieldsTuple = std::tuple<Fields...>;

    template <type::NameTag N>
    using ResolvedField = typename find_field<N, AllFieldsTuple>::type;

    static_assert((!std::is_void_v<ResolvedField<SelectedFields>> && ...),
      "One or more requested fields were not found in the AccessorList.");

    static constexpr size_t raw_size = (ResolvedField<SelectedFields>::size + ... + 0);

  public:
    static constexpr size_t SIZE = (raw_size + 7) & ~7;

    explicit Builder(const T& source) : _source(source) {}

    void make(std::byte* destination) const noexcept {
      std::byte* current = destination;

      ([&]() {
        using F = ResolvedField<SelectedFields>;
        typename F::inserter_type()(this->_source, current);
        current += F::size;
      }(), ...);

      size_t bytes_written = current - destination;
      if (bytes_written < SIZE) {
        std::memset(current, 0, SIZE - bytes_written);
      }
    }

    static constexpr bool matchList(std::string_view list) noexcept {
      return KeyBuilder::matchTags<SelectedFields...>(list);
    }

  private:
    const T& _source;
  };

  // -------------------------------------------------------------------------
  // PaddedBuilder: Constructs key using ALL fields, in TypeList definition order.
  // Selected fields are copied; unselected fields are padded.
  // -------------------------------------------------------------------------
  template <type::NameTag... SelectedFields>
  class PaddedBuilder {
    // Identify which fields are selected
    template <type::NameTag N>
    static constexpr bool IsSelected = ((N.toString() == SelectedFields.toString()) || ...);

    // Calculate total size from the definition list
    static constexpr size_t raw_size = (Fields::size + ... + 0);

  public:
    static constexpr size_t SIZE = (raw_size + 7) & ~7;

    explicit PaddedBuilder(const T& source) : _source(source) {}

    void make(std::byte* destination) const noexcept {
      std::byte* current = destination;

      // Iterate over ALL fields in definition order
      ([&]() {
        using F = Fields;
        if constexpr (IsSelected<F::name>) {
          typename F::inserter_type()(this->_source, current);
        } else {
          constexpr std::byte pad = get_default_byte<F>::value;
          std::memset(current, static_cast<int>(pad), F::size);
        }
        current += F::size;
      }(), ...);

      size_t bytes_written = current - destination;
      if (bytes_written < SIZE) {
        std::memset(current, 0, SIZE - bytes_written);
      }
    }

    static constexpr bool matchList(std::string_view list) noexcept {
      return KeyBuilder::matchTags<SelectedFields...>(list);
    }

  private:
    const T& _source;
  };
};

}
