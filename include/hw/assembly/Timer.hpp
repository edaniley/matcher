#pragma once
#include <chrono>
#include <functional>
#include <iostream>
#include <hw/utility/PriorityQueue.hpp>

namespace hw::assembly {

using namespace std::chrono;

enum class TimerType {
  ONE_TIME = 1,
  RECURRING = 2
};

template <size_t N>
class TimerQueue {
public:
  bool scheduleAt(system_clock::time_point when, std::function<void()>callback) noexcept {
    const microseconds usec = duration_cast<microseconds>(when - system_clock::now());
    return _queue.push(TimerEvent{TimerType::ONE_TIME, when, usec, std::move(callback)});
  }

  template <typename Rep, typename Period>
  bool scheduleAfter(TimerType type, duration<Rep, Period> wait, std::function<void()> callback) noexcept {
    const system_clock::time_point when = system_clock::now() + wait;
    const microseconds usec = duration_cast<microseconds> (wait);
    return _queue.push(TimerEvent{type, when, usec, std::move(callback)});
  }

  size_t poll() noexcept {
    size_t executed = 0;
    const auto now = system_clock::now();

    while (!_queue.empty() && _queue.top().when <= now) {
      auto & top = _queue.top();
      top.callback();
      ++ executed;

      if (top.type == TimerType::RECURRING) {
        const system_clock::time_point when = system_clock::now() + top.wait;
        _queue.push(TimerEvent{top.type, when, top.wait, std::move(top.callback)});
      }

      _queue.pop() ;
    }
    return executed;
  }

  system_clock::time_point next() const noexcept {
    return _queue.empty() ? system_clock:: time_point::max() : _queue. top().when;
  }

  bool empty() const noexcept {
    return _queue.empty();
  }

  void clear() {
    _queue.clear();
  }

private:
  struct TimerEvent {
    bool operator < (const TimerEvent & other) const {
      return when > other. when;
    }

    TimerType type;
    system_clock::time_point when;
    microseconds wait;
    std::function<void()> callback;
  };

  utility::PriorityQueue<TimerEvent, N> _queue;
};

}



