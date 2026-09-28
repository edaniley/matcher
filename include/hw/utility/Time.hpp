#pragma once

#include <cstdint>
#include <chrono>
#include <type_traits>
#include <string>
#include <iomanip>
#include <sstream>

#include <hw/utility/Format.hpp>

namespace hw::utility {

using Timestamp = int64_t; // Nanoseconds since Epoch
using Timepoint = std::chrono::system_clock::time_point;


template <typename T>
struct DurationType : std::false_type {};

template <typename Rep, typename Period>
struct DurationType<std::chrono::duration<Rep, Period>> : std::true_type {};

template <typename T>
inline constexpr bool IsDuration = DurationType<T>::value;


template <typename Duration>
constexpr Timepoint RoundTimepoint(const Timepoint& timepoint, const Duration& duration) {
    const auto bucketCnt = std::chrono::duration_cast<Duration>(timepoint.time_since_epoch());
    return Timepoint(bucketCnt - (bucketCnt % duration));
}

static_assert(RoundTimepoint(Timepoint(std::chrono::seconds(17)), std::chrono::seconds (15)) ==
              Timepoint (std::chrono::seconds (15)));


/**
 * --- Conversions ---
 */

template <typename Duration>
constexpr Duration DurationFromNanoseconds(int64_t nanoseconds) {
  return std::chrono::duration_cast<Duration>(std::chrono::nanoseconds(nanoseconds)) ;
}

template <typename Duration>
constexpr Timestamp DurationToNanoseconds(const Duration& duration) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}
static_assert(DurationToNanoseconds(std::chrono::seconds(42)) == 42'000'000'000);

constexpr Timestamp TimepointToNanoseconds(const Timepoint& timepoint) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(timepoint.time_since_epoch()).count();
}

constexpr Timepoint TimepointFromNanoseconds(Timestamp nanoseconds) {
    return Timepoint(std::chrono::nanoseconds(nanoseconds));
}

static_assert(TimepointToNanoseconds(TimepointFromNanoseconds(1748908800000000000)) == 1748908800000000000);

// Format: 2026-01-04 12:00:00.000000000
inline std::string TimestampToString(Timestamp nanoseconds, bool local = false) {
    std::time_t sec = nanoseconds / 1'000'000'000;
    int32_t nanos = static_cast<int32_t>(nanoseconds % 1'000'000'000);

    std::tm tm_struct;
    if (local) {
        localtime_r(&sec, &tm_struct);
    } else {
        gmtime_r(&sec, &tm_struct);
    }

    return frmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:09}{}",
                        tm_struct.tm_year + 1900, tm_struct.tm_mon + 1, tm_struct.tm_mday,
                        tm_struct.tm_hour, tm_struct.tm_min, tm_struct.tm_sec,
                        nanos, local ? "" : " UTC");
}

inline std::string TimepointToString(const Timepoint& timepoint, bool local = false) {
    return TimestampToString(TimepointToNanoseconds(timepoint), local);
}

/**
 * --- Parsing ---
 */
inline Timepoint TimepointFromString(const std::string& str, const char* format = "%Y-%m-%d %H:%M:%S") {
    std::tm t{};
    std::istringstream ss(str);
    ss >> std::get_time(&t, format);
    if (ss.fail()) {
        throw std::runtime_error(frmt::format("Failed to parse time string: {}", str));
    }

    // mktime assumes local time
    std::time_t time = std::mktime(&t);
    if (time == -1) {
        throw std::runtime_error("mktime failed");
    }
    return std::chrono::system_clock::from_time_t(time);
}

}
