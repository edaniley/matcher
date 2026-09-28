#pragma once

#include <string>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <charconv>
#include <stdexcept>
#include <type_traits>
#include <regex>
#include <vector>
#include <cstring>
#include <random>
#include <algorithm>
#include <string_view>

namespace hw::utility {

[[nodiscard]]
inline constexpr bool containsWhitespace(std::string_view str) {
  return std::any_of(str.begin(), str.end(), [](unsigned char ch) {
    return std::isspace(ch);
  });
}

inline constexpr bool stringsEqual(char const* a, char const* b) {
  return *a == *b && (*a == '\0' || stringsEqual(a + 1, b + 1));
}

inline constexpr size_t stringLen(const char* str) {
  return *str ? 1 + stringLen(str + 1) : 0;
}

[[nodiscard]]
inline bool isBlankOrEmpty(std::string_view str) {
  return std::all_of(str.begin(), str.end(), [](unsigned char ch) {
    return std::isspace(ch);
  });
}

[[nodiscard]]
inline std::string ltrim(const std::string& str) {
  auto it = std::find_if(str.begin(), str.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  });
  return {it, str.end()};
}

inline std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  auto end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

struct SplitOptions {
  bool trim_tokens = true;
  bool include_empty = false;
};

[[nodiscard]]
inline std::vector<std::string> splitString(const std::string& s, char delimiter, SplitOptions split_opts = SplitOptions()) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream tokenStream(s);
  while (std::getline(tokenStream, token, delimiter)) {
    if (split_opts.trim_tokens) token = trim(token);
    if (!token.empty() || split_opts.include_empty) {
      tokens.push_back(token);
    }
  }
  return tokens;
}

[[nodiscard]]
inline std::vector<std::string> splitString(const std::string& s, const std::string& delimiter, SplitOptions split_opts = SplitOptions()) {
  std::vector<std::string> tokens;
  std::size_t start = 0, end = 0;
  while ((end = s.find(delimiter, start)) != std::string::npos) {
    auto token = s.substr(start, end - start);
    if (split_opts.trim_tokens) token = trim(token);
    if (!token.empty() || split_opts.include_empty) {
      tokens.emplace_back(token);
    }
    start = end + delimiter.length();
  }
  auto lastToken = s.substr(start);
  if (split_opts.trim_tokens) lastToken = trim(lastToken);
  if (!lastToken.empty() || split_opts.include_empty) {
    tokens.emplace_back(lastToken);
  }
  return tokens;
}

inline std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

template <typename T>
T fromString(const std::string& str) {
  if constexpr (std::is_same_v<T, std::string>) {
    return str;
  }
  else if constexpr (std::is_same_v<T, bool>) {
    const std::string s = trim(toLower(str));
    if (s == "true" || s == "1") return true;
    if (s == "false" || s == "0") return false;
    throw std::invalid_argument("Invalid boolean value: '" + str + "'");
  }
  else if constexpr (std::is_integral_v<T>) {
    T value;
    int base = 10;
    const char* begin = str.data();
    const char* end = begin + str.size();
    if ((str.size() > 2) && (str[0] == '0') && (std::tolower(str[1]) == 'x')) {
      base = 16;
      begin += 2;
    }
    auto [ptr, ec] = std::from_chars(begin, end, value, base);
    if (ec != std::errc() || ptr != end) {
      throw std::invalid_argument("Invalid integral conversion for: " + str);
    }
    return value;
  }
  else if constexpr (std::is_floating_point_v<T>) {
    T value;
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
    if (ec != std::errc() || ptr != (str.data() + str.size())) {
      throw std::invalid_argument("Invalid numeric conversion for: " + str);
    }
    return value;
  }
  else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
    std::tm tm = {};
    std::istringstream iss(str);
    iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (iss.fail()) {
      throw std::invalid_argument("Invalid time point format (expected ISO 8601): " + str);
    }
    // Portable C++20 way to convert UTC tm to time_point
    auto tp = std::chrono::sys_days{std::chrono::year{tm.tm_year + 1900} / (tm.tm_mon + 1) / tm.tm_mday} +
              std::chrono::hours{tm.tm_hour} + std::chrono::minutes{tm.tm_min} + std::chrono::seconds{tm.tm_sec};
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(tp);
  }
  else if constexpr (std::is_convertible_v<T, std::chrono::nanoseconds>) {
    static const std::regex duration_regex(R"((\d+)(h|millis|msec|micros|min|ms|m|sec|s|usec|us|d))");
    auto it = std::sregex_iterator(str.begin(), str.end(), duration_regex);
    auto end = std::sregex_iterator();
    if (it == end) throw std::invalid_argument("Invalid duration format: " + str);

    std::chrono::nanoseconds total(0);
    for (; it != end; ++it) {
      long long value = std::stoll((*it)[1]);
      std::string_view unit = (*it)[2].str();
      if (unit == "h") total += std::chrono::hours(value);
      else if (unit == "min" || unit == "m") total += std::chrono::minutes(value);
      else if (unit == "sec" || unit == "s") total += std::chrono::seconds(value);
      else if (unit == "millis" || unit == "msec" || unit == "ms") total += std::chrono::milliseconds(value);
      else if (unit == "micros" || unit == "usec" || unit == "us") total += std::chrono::microseconds(value);
      else if (unit == "d") total += std::chrono::hours(value * 24);
    }
    return std::chrono::duration_cast<T>(total);
  }
  else {
    static_assert(sizeof(T) == 0, "Unsupported type for conversion");
  }
}

[[nodiscard]]
inline std::string toHex(const void* mem, size_t memsz, size_t width = 32, bool addtext = true) {
  if (!mem || memsz == 0) return "";
  std::ostringstream oss;
  const uint8_t* tail = static_cast<const uint8_t*>(mem);

  while (memsz > 0) {
    const size_t len = std::min(width, memsz);

    // Hex section
    for (size_t col = 0; col < len; ++col) {
      oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(tail[col]) << " ";
    }

    // Padding for short lines to keep the text section aligned
    if (len < width) {
      oss << std::string(3 * (width - len), ' ');
    }

    // ASCII text section
    if (addtext) {
      oss << " ";
      for (size_t col = 0; col < len; ++col) {
        const char c = static_cast<char>(tail[col]);
        oss << (c >= 32 && c <= 126 ? c : '.');
      }
    }
    oss << "\n";
    memsz -= len;
    tail += len;
  }
  return oss.str();
}

inline std::string randomString(size_t length) {
  static constexpr char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  thread_local std::mt19937 gen(std::random_device{}());
  std::uniform_int_distribution<> dist(0, sizeof(chars) - 2);
  std::string s(length, '\0');
  std::generate_n(s.begin(), length, [&]() { return chars[dist(gen)]; });
  return s;
}

inline uint8_t toNibble(char in) {
  if (in >= '0' && in <= '9') return static_cast<uint8_t>(in - '0');
  if (in >= 'a' && in <= 'f') return static_cast<uint8_t>(in - 'a' + 10);
  if (in >= 'A' && in <= 'F') return static_cast<uint8_t>(in - 'A' + 10);
  throw std::invalid_argument("Invalid hex character");
}

// https://stackoverflow.com/questions/16388510/evaluate-a-string-with-a-switch-in-c
constexpr unsigned int str2int(const char* str, int h = 0) {
  return !str[h] ? 5381 : (str2int(str, h + 1) * 33) ^ static_cast<unsigned char>(str[h]);
}

// https://stackoverflow.com/questions/38955940/how-to-concatenate-static-strings-at-compile-time/62823211#62823211
namespace detail {
  template <std::string_view const&... Strs>
  struct join {
    static constexpr auto impl() noexcept {
      constexpr std::size_t len = (Strs.size() + ... + 0);
      std::array<char, len + 1> arr{};
      auto append = [i = 0, &arr](auto const& s) mutable {
        for (auto c : s) arr[i++] = c;
      };
      (append(Strs), ...);
      arr[len] = 0;
      return arr;
    }
    static constexpr auto arr = impl();
    static constexpr std::string_view value {arr.data(), arr.size() - 1};
  };
}

template <std::string_view const&... Strs>
static constexpr auto JoinStrings = detail::join<Strs...>::value;

}
