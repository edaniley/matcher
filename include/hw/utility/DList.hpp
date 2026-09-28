#pragma once
#include <cassert>
#include <cstddef>
#include <iterator>
#include <type_traits>

namespace hw::utility {

template <typename Type> class DListHook;
template <typename Type, DListHook<Type> Type::* HookMember> class DList;

//
// DListHook: link storage embedded in a Type, making it a member of an intrusive DList.
// A Type may have several hooks (one per list it can be in at the same time):
//
//   struct Order {
//     DListHook<Order> levelHook;     // position in price level queue
//     DListHook<Order> accountHook;   // position in per-account list
//   };
//   DList<Order, &Order::levelHook>   level;
//   DList<Order, &Order::accountHook> byAccount;
//
// - No allocation: the list never owns or allocates elements; the caller manages lifetime.
// - Copying a hook yields an unlinked hook, so the enclosing Type stays copyable and a copy
//   never inherits list membership. Assignment leaves the target's links untouched.
// - An element must be unlinked (removed from its list) before it is destroyed.
//
template <typename Type>
class DListHook {
public:
  DListHook() noexcept = default;
  DListHook(const DListHook &) noexcept {}
  DListHook & operator = (const DListHook &) noexcept { return *this; }
  ~DListHook() { assert(!isLinked() && "element destroyed while still in a DList"); }

  bool isLinked() const noexcept { return _owner != nullptr; }

private:
  template <typename T, DListHook<T> T::* HookMember> friend class DList;

  Type *       _prev  = nullptr;
  Type *       _next  = nullptr;
  const void * _owner = nullptr;   // list this hook is linked into; nullptr when unlinked
};

//
// DList: intrusive doubly linked list threaded through Type::*HookMember.
// All operations are O(1) except clear() and the destructor (O(n) unlink).
// Not thread safe. Not copyable or movable (elements point back at the list).
//
template <typename Type, DListHook<Type> Type::* HookMember>
class DList {
  static DListHook<Type> & hook(Type & obj) noexcept { return obj.*HookMember; }
  static const DListHook<Type> & hook(const Type & obj) noexcept { return obj.*HookMember; }

  template <bool CONST>
  class Iterator {
    using ListPtr = std::conditional_t<CONST, const DList *, DList *>;
    using NodePtr = std::conditional_t<CONST, const Type *, Type *>;
  public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type        = Type;
    using difference_type   = std::ptrdiff_t;
    using pointer           = NodePtr;
    using reference         = std::conditional_t<CONST, const Type &, Type &>;

    Iterator() noexcept = default;
    Iterator(ListPtr list, NodePtr node) noexcept : _list(list), _node(node) {}
    // iterator -> const_iterator
    template <bool C = CONST> requires C
    Iterator(const Iterator<false> & other) noexcept : _list(other._list), _node(other._node) {}

    reference operator *  () const noexcept { return *_node; }
    pointer   operator -> () const noexcept { return _node; }

    Iterator & operator ++ () noexcept { _node = hook(*_node)._next; return *this; }
    Iterator & operator -- () noexcept { _node = _node ? hook(*_node)._prev : _list->_tail; return *this; }
    Iterator   operator ++ (int) noexcept { Iterator tmp = *this; ++*this; return tmp; }
    Iterator   operator -- (int) noexcept { Iterator tmp = *this; --*this; return tmp; }

    friend bool operator == (const Iterator & a, const Iterator & b) noexcept { return a._node == b._node; }

  private:
    friend class DList;
    template <bool> friend class Iterator;
    ListPtr _list = nullptr;
    NodePtr _node = nullptr;     // nullptr == end()
  };

public:
  using value_type      = Type;
  using reference       = Type &;
  using const_reference = const Type &;
  using size_type       = size_t;
  using iterator        = Iterator<false>;
  using const_iterator  = Iterator<true>;
  using reverse_iterator       = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  DList() noexcept = default;
  ~DList() { clear(); }

  DList(const DList &) = delete;
  DList & operator = (const DList &) = delete;

  bool      empty() const noexcept { return _size == 0; }
  size_type size()  const noexcept { return _size; }

  Type &       front()       noexcept { assert(!empty()); return *_head; }
  const Type & front() const noexcept { assert(!empty()); return *_head; }
  Type &       back()        noexcept { assert(!empty()); return *_tail; }
  const Type & back()  const noexcept { assert(!empty()); return *_tail; }

  // true if obj is linked into this particular list
  bool contains(const Type & obj) const noexcept { return hook(obj)._owner == this; }

  void push_front(Type & obj) noexcept { insert(begin(), obj); }
  void push_back (Type & obj) noexcept { insert(end(), obj); }

  void pop_front() noexcept { assert(!empty()); erase(*_head); }
  void pop_back () noexcept { assert(!empty()); erase(*_tail); }

  // links obj before pos; obj must not be linked into any list (for this hook)
  iterator insert(const_iterator pos, Type & obj) noexcept {
    assert(pos._list == this);
    DListHook<Type> & h = hook(obj);
    assert(!h.isLinked());
    Type * next = const_cast<Type *>(pos._node);
    Type * prev = next ? hook(*next)._prev : _tail;
    h._prev  = prev;
    h._next  = next;
    h._owner = this;
    (prev ? hook(*prev)._next : _head) = &obj;
    (next ? hook(*next)._prev : _tail) = &obj;
    ++_size;
    return iterator(this, &obj);
  }

  // unlinks obj; returns iterator to the element that followed it
  iterator erase(Type & obj) noexcept {
    assert(contains(obj));
    DListHook<Type> & h = hook(obj);
    Type * prev = h._prev;
    Type * next = h._next;
    (prev ? hook(*prev)._next : _head) = next;
    (next ? hook(*next)._prev : _tail) = prev;
    h._prev  = nullptr;
    h._next  = nullptr;
    h._owner = nullptr;
    --_size;
    return iterator(this, next);
  }

  iterator erase(const_iterator pos) noexcept {
    assert(pos._list == this && pos._node);
    return erase(*const_cast<Type *>(pos._node));
  }

  // unlinks all elements (elements themselves are untouched otherwise)
  void clear() noexcept {
    while (_head) {
      DListHook<Type> & h = hook(*_head);
      Type * next = h._next;
      h._prev  = nullptr;
      h._next  = nullptr;
      h._owner = nullptr;
      _head = next;
    }
    _tail = nullptr;
    _size = 0;
  }

  // iterator for an element known to be in this list; O(1)
  iterator       iterator_to(Type & obj) noexcept             { assert(contains(obj)); return iterator(this, &obj); }
  const_iterator iterator_to(const Type & obj) const noexcept { assert(contains(obj)); return const_iterator(this, &obj); }

  iterator       begin()        noexcept { return iterator(this, _head); }
  iterator       end()          noexcept { return iterator(this, nullptr); }
  const_iterator begin()  const noexcept { return const_iterator(this, _head); }
  const_iterator end()    const noexcept { return const_iterator(this, nullptr); }
  const_iterator cbegin() const noexcept { return begin(); }
  const_iterator cend()   const noexcept { return end(); }

  reverse_iterator       rbegin()       noexcept { return reverse_iterator(end()); }
  reverse_iterator       rend()         noexcept { return reverse_iterator(begin()); }
  const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
  const_reverse_iterator rend()   const noexcept { return const_reverse_iterator(begin()); }

private:
  Type *    _head = nullptr;
  Type *    _tail = nullptr;
  size_type _size = 0;
};

}
