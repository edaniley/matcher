#pragma once
#include <functional>
#include <algorithm>
#include <utility>
#include <cassert>

namespace hw::utility {

//
// simple priority queue; not thread safe
//
template <typename T, size_t N, typename Compare = std::less<T>>
class PriorityQueue {
public:
  explicit PriorityQueue(const Compare& comp = Compare()) : _size(0), _comp(comp) {}

  bool push(const T & value) {
    if (_size >= N) return false;
    size_t i = _size;
    _heap[_size++] = value;
    heapifyUp (i);
    return true;
  }

  bool push(T && value) {
    if (_size >= N) return false;
    size_t i = _size;
    _heap[_size++] = std::move(value);
    heapifyUp (i);
    return true;
  }

  template<typename... Args>
  bool emplace(Args&&... args) {
    if (_size >= N) return false;
    size_t i = _size;
    _heap[_size++] = T(std::forward<Args>(args)...);
    heapifyUp (i);
    return true;
  }

  const T & top() const {
    assert(_size > 0);
    return _heap[0];
  }

  void pop() {
    if (_size == 0)
      return;

    _heap[0] = std::move(_heap[--_size]);
    heapifyDown(0);
  }

  size_t size () const  { return _size; }
  bool empty  () const  { return _size == 0; }
  void clear  ()        { _size = 0; }

private:
  void heapifyUp (size_t i) {
    while (i > 0) {
      size_t parent = (i - 1) / 2;
      if (_comp(_heap[parent], _heap[i])) {
        std::swap(_heap[i], _heap[parent]);
        i = parent;
      } else {
        break;
      }
    }
  }

  void heapifyDown(size_t i) {
    while (true) {
      size_t largest = i;
      size_t left = 2 * i + 1;
      size_t right = 2 * i + 2;
      if (left < _size && _comp(_heap[largest], _heap[left])) {
        largest = left;
      }
      if (right < _size && _comp(_heap[largest], _heap[right])) {
        largest = right;
      }
      if (largest == i)
        break;

      std::swap(_heap[i], _heap[largest]);
      i = largest;
    }
  }

  T       _heap[N];
  size_t  _size;
  Compare _comp;
};

}
