#pragma once
#if __GNUC__ < 13
#define FMT_HEADER_ONLY
#include <fmt/core.h>
#include <fmt/chrono.h>
namespace frmt = fmt;
#else
#include <format>
namespace frmt = std;
#endif
