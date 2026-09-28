#pragma once
#include <sched.h>
#include <unistd.h>
#include <fstream>
#include <set>
#include <ranges>
#include <algorithm>
#include <new>

#include <hw/utility/Text.hpp>

namespace hw::utility {

#define ALIGNAS (64 * 1)
// std::hardware_constructive_interference_size

#define breakpoint() asm ("int $3")

[[nodiscard]] inline std::set<int> getIsolatedCpuList() {
  std::set<int> cores;

  auto listproc = [&cores](const std:: string & line) {
    for (auto & token : splitString(line, ',')) {
      size_t z = token.find('-');
      if (z == std::string:: npos) {
        cores.insert(fromString<int>(token)) ;
      } else {
        int lower = fromString<int>(token.substr(0, z));
        int upper = fromString<int>(token.substr(z + 1)) + 1;
        cores.insert(std::views::iota(lower, upper).begin(), std::views::iota(lower, upper).end());
      }
    }
  };

  try {
    const char* tuned = "/etc/tuned/cpu-partitioning-variables.conf";
    std::ifstream file(tuned);
    if (file. is_open()) {
      std::string line;
      while (std::getline(file, line)) {
        if (line.find ("isolated_cores") != std::string::npos) {
          listproc(line.substr(line.find('=') + 1));
          break;
        }
      }
    }
  }
  catch (...) { }

	if (cores. empty()) {
    const char* classic = "/sys/devices/system/cpu/isolated";
    std::ifstream file(classic);
    if (file.is_open()) {
      std::string line;
      std::getline(file, line);
      listproc (line);
      file.close() ;
    }
  }
	return cores;
}


[[nodiscard]] inline std::set<int> getCpuAffinity() {
  cpu_set_t cpuset;
  CPU_ZERO (&cpuset);
  std::set<int> cores;
  if (sched_getaffinity(0, sizeof (cpu_set_t), &cpuset) == 0) {
    std::ranges::for_each(std::views::iota(0, CPU_SETSIZE),[&cores, &cpuset] (int core) {
      if (CPU_ISSET(core, &cpuset)) {
        cores. insert (core);
      }
    });
  }
  return cores;
}

[[nodiscard]] inline int setCpuAffinity(int core) {
  cpu_set_t cpuset;
  CPU_ZERO (&cpuset);
  CPU_SET (core, &cpuset);
  return sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

// Sets affinity to all available cores except isolated ones; can be used for non-critical threads.
// But needs to be used with care as some hosts may have reserved cores, inherit thread priorities etc.
// prefer selecting the core explicitly.
[[nodiscard]] inline bool resetCpuAffinity() {
  cpu_set_t cpuset;
  CPU_ZERO (&cpuset) ;
  if (long available = sysconf(_SC_NPROCESSORS_ONLN); available > 1) {
    std::set<int> isolated(getIsolatedCpuList());
    std::ranges::for_each(std::views::iota(3, available), [&isolated, &cpuset] (int core) {
      if (isolated.find(core) == isolated.end()) {
        CPU_SET (core, &cpuset);
      }
    });
    return (0 == sched_setaffinity(0, sizeof(cpuset), &cpuset));
  }
  return false;
}


}
