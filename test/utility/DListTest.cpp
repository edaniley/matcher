#include <boost/test/unit_test.hpp>
#include <hw/utility/DList.hpp>
#include <algorithm>
#include <iterator>
#include <list>
#include <memory>
#include <random>
#include <vector>

using hw::utility::DList;
using hw::utility::DListHook;

namespace {

struct Node {
  int id = 0;
  DListHook<Node> hookA;
  DListHook<Node> hookB;
  explicit Node(int i = 0) : id(i) {}
};

using ListA = DList<Node, &Node::hookA>;
using ListB = DList<Node, &Node::hookB>;
using V = std::vector<int>;

template <typename List>
V ids(const List & list) {
  V out;
  for (const Node & n : list) out.push_back(n.id);
  return out;
}

template <typename List>
V reverseIds(const List & list) {
  V out;
  for (auto it = list.rbegin(); it != list.rend(); ++it) out.push_back(it->id);
  return out;
}

// structural check: forward and backward traversal agree, size matches, every element owned
template <typename List>
void checkInvariants(const List & list) {
  V fwd = ids(list);
  V bwd = reverseIds(list);
  std::reverse(bwd.begin(), bwd.end());
  BOOST_REQUIRE(fwd == bwd);
  BOOST_REQUIRE_EQUAL(fwd.size(), list.size());
  BOOST_REQUIRE_EQUAL(list.empty(), fwd.empty());
  for (const Node & n : list) BOOST_REQUIRE(list.contains(n));
  if (!list.empty()) {
    BOOST_REQUIRE_EQUAL(list.front().id, fwd.front());
    BOOST_REQUIRE_EQUAL(list.back().id, fwd.back());
  }
}

// fixture with a pool of nodes; unlinks everything before nodes are destroyed
struct Fixture {
  static constexpr int N = 8;
  std::vector<std::unique_ptr<Node>> nodes;
  ListA la;
  ListB lb;

  Fixture() {
    for (int i = 0; i < N; ++i) nodes.push_back(std::make_unique<Node>(i));
  }
  ~Fixture() {
    la.clear();
    lb.clear();
  }
  Node & operator [] (int i) { return *nodes[i]; }
};

}

static_assert(std::bidirectional_iterator<ListA::iterator>);
static_assert(std::bidirectional_iterator<ListA::const_iterator>);
static_assert(std::is_convertible_v<ListA::iterator, ListA::const_iterator>);
static_assert(!std::is_convertible_v<ListA::const_iterator, ListA::iterator>);
static_assert(!std::is_copy_constructible_v<ListA>);
static_assert(!std::is_move_constructible_v<ListA>);
static_assert(std::is_copy_constructible_v<Node>);        // hook keeps host copyable

BOOST_AUTO_TEST_SUITE(DListTests)

// ---------------------------------------------------------------- hook

BOOST_AUTO_TEST_CASE(HookDefaultUnlinked) {
  Node a(1);
  BOOST_CHECK(!a.hookA.isLinked());
  BOOST_CHECK(!a.hookB.isLinked());
}

BOOST_FIXTURE_TEST_CASE(HookLinkedStateFollowsList, Fixture) {
  la.push_back((*this)[0]);
  BOOST_CHECK((*this)[0].hookA.isLinked());
  BOOST_CHECK(!(*this)[0].hookB.isLinked());
  la.pop_back();
  BOOST_CHECK(!(*this)[0].hookA.isLinked());
}

BOOST_FIXTURE_TEST_CASE(CopyConstructedHostIsUnlinked, Fixture) {
  la.push_back((*this)[0]);
  Node copy = (*this)[0];
  BOOST_CHECK_EQUAL(copy.id, 0);
  BOOST_CHECK(!copy.hookA.isLinked());
  BOOST_CHECK(!la.contains(copy));
  BOOST_CHECK_EQUAL(la.size(), 1u);
}

BOOST_FIXTURE_TEST_CASE(CopyAssignmentKeepsTargetLinks, Fixture) {
  Node & a = (*this)[0];
  Node & b = (*this)[1];
  la.push_back(a);
  la.push_back(b);
  b.id = 42;
  a = b;                                   // copies payload, not links
  BOOST_CHECK_EQUAL(a.id, 42);
  BOOST_CHECK(la.contains(a) && la.contains(b));
  BOOST_CHECK_EQUAL(la.size(), 2u);
  checkInvariants(la);

  Node loose(7);
  loose = a;                               // unlinked target stays unlinked
  BOOST_CHECK(!loose.hookA.isLinked());
}

// ---------------------------------------------------------------- empty list

BOOST_AUTO_TEST_CASE(EmptyList) {
  ListA list;
  const ListA & clist = list;
  BOOST_CHECK(list.empty());
  BOOST_CHECK_EQUAL(list.size(), 0u);
  BOOST_CHECK(list.begin() == list.end());
  BOOST_CHECK(clist.begin() == clist.end());
  BOOST_CHECK(list.cbegin() == list.cend());
  BOOST_CHECK(list.rbegin() == list.rend());
  BOOST_CHECK_EQUAL(std::distance(list.begin(), list.end()), 0);
  list.clear();                            // clear on empty is a no-op
  BOOST_CHECK(list.empty());
}

// ---------------------------------------------------------------- push / pop

BOOST_FIXTURE_TEST_CASE(SingleElement, Fixture) {
  Node & a = (*this)[0];
  la.push_back(a);
  BOOST_CHECK_EQUAL(la.size(), 1u);
  BOOST_CHECK_EQUAL(&la.front(), &a);
  BOOST_CHECK_EQUAL(&la.back(), &a);
  checkInvariants(la);
  la.pop_front();
  BOOST_CHECK(la.empty());
  la.push_front(a);
  BOOST_CHECK_EQUAL(&la.back(), &a);
  la.pop_back();
  BOOST_CHECK(la.empty());
}

BOOST_FIXTURE_TEST_CASE(PushBackKeepsFifoOrder, Fixture) {
  for (int i = 0; i < N; ++i) la.push_back((*this)[i]);
  BOOST_CHECK(ids(la) == (V{0, 1, 2, 3, 4, 5, 6, 7}));
  BOOST_CHECK(reverseIds(la) == (V{7, 6, 5, 4, 3, 2, 1, 0}));
  checkInvariants(la);
}

BOOST_FIXTURE_TEST_CASE(PushFrontReversesOrder, Fixture) {
  for (int i = 0; i < 4; ++i) la.push_front((*this)[i]);
  BOOST_CHECK(ids(la) == (V{3, 2, 1, 0}));
  checkInvariants(la);
}

BOOST_FIXTURE_TEST_CASE(MixedPushPop, Fixture) {
  la.push_back((*this)[1]);
  la.push_front((*this)[0]);
  la.push_back((*this)[2]);
  BOOST_CHECK(ids(la) == (V{0, 1, 2}));
  la.pop_front();
  BOOST_CHECK(!(*this)[0].hookA.isLinked());
  BOOST_CHECK(ids(la) == (V{1, 2}));
  la.pop_back();
  BOOST_CHECK(!(*this)[2].hookA.isLinked());
  BOOST_CHECK(ids(la) == (V{1}));
  checkInvariants(la);
}

BOOST_FIXTURE_TEST_CASE(FrontBackMutable, Fixture) {
  la.push_back((*this)[0]);
  la.push_back((*this)[1]);
  la.front().id = 100;
  la.back().id = 200;
  BOOST_CHECK(ids(la) == (V{100, 200}));
  const ListA & c = la;
  BOOST_CHECK_EQUAL(c.front().id, 100);
  BOOST_CHECK_EQUAL(c.back().id, 200);
}

// ---------------------------------------------------------------- insert

BOOST_FIXTURE_TEST_CASE(InsertAtBeginMiddleEnd, Fixture) {
  la.push_back((*this)[1]);
  la.push_back((*this)[3]);
  auto it = la.insert(la.begin(), (*this)[0]);                 // head
  BOOST_CHECK_EQUAL(it->id, 0);
  it = la.insert(la.iterator_to((*this)[3]), (*this)[2]);      // middle
  BOOST_CHECK_EQUAL(it->id, 2);
  it = la.insert(la.end(), (*this)[4]);                        // tail
  BOOST_CHECK_EQUAL(it->id, 4);
  BOOST_CHECK(ids(la) == (V{0, 1, 2, 3, 4}));
  checkInvariants(la);
}

BOOST_FIXTURE_TEST_CASE(InsertIntoEmptyViaEnd, Fixture) {
  la.insert(la.end(), (*this)[5]);
  BOOST_CHECK(ids(la) == (V{5}));
  la.insert(la.cbegin(), (*this)[4]);     // const_iterator position
  BOOST_CHECK(ids(la) == (V{4, 5}));
  checkInvariants(la);
}

// ---------------------------------------------------------------- erase

BOOST_FIXTURE_TEST_CASE(EraseHeadMiddleTail, Fixture) {
  for (int i = 0; i < 5; ++i) la.push_back((*this)[i]);
  auto next = la.erase((*this)[2]);
  BOOST_CHECK_EQUAL(next->id, 3);
  BOOST_CHECK(ids(la) == (V{0, 1, 3, 4}));
  next = la.erase((*this)[0]);
  BOOST_CHECK_EQUAL(next->id, 1);
  BOOST_CHECK_EQUAL(la.front().id, 1);
  next = la.erase((*this)[4]);
  BOOST_CHECK(next == la.end());
  BOOST_CHECK_EQUAL(la.back().id, 3);
  BOOST_CHECK(ids(la) == (V{1, 3}));
  checkInvariants(la);
  for (int i : {0, 2, 4}) BOOST_CHECK(!(*this)[i].hookA.isLinked());
}

BOOST_FIXTURE_TEST_CASE(EraseByIterator, Fixture) {
  for (int i = 0; i < 4; ++i) la.push_back((*this)[i]);
  auto it = std::next(la.begin());
  it = la.erase(it);
  BOOST_CHECK_EQUAL(it->id, 2);
  ListA::const_iterator cit = la.cbegin();
  it = la.erase(cit);
  BOOST_CHECK_EQUAL(it->id, 2);
  BOOST_CHECK(ids(la) == (V{2, 3}));
  checkInvariants(la);
}

BOOST_FIXTURE_TEST_CASE(EraseWhileIterating, Fixture) {
  for (int i = 0; i < N; ++i) la.push_back((*this)[i]);
  for (auto it = la.begin(); it != la.end(); ) {
    it = (it->id % 2 == 0) ? la.erase(it) : std::next(it);
  }
  BOOST_CHECK(ids(la) == (V{1, 3, 5, 7}));
  checkInvariants(la);
}

BOOST_FIXTURE_TEST_CASE(ReinsertAfterErase, Fixture) {
  Node & a = (*this)[0];
  la.push_back(a);
  la.push_back((*this)[1]);
  la.erase(a);
  la.push_back(a);                         // moves to the back of the queue
  BOOST_CHECK(ids(la) == (V{1, 0}));
  checkInvariants(la);
}

BOOST_FIXTURE_TEST_CASE(MoveBetweenListsOfSameHook, Fixture) {
  ListA other;
  Node & a = (*this)[0];
  la.push_back(a);
  la.erase(a);
  other.push_back(a);
  BOOST_CHECK(!la.contains(a));
  BOOST_CHECK(other.contains(a));
  other.clear();
}

// ---------------------------------------------------------------- clear / destruction

BOOST_FIXTURE_TEST_CASE(ClearUnlinksAllAndListIsReusable, Fixture) {
  for (int i = 0; i < N; ++i) la.push_back((*this)[i]);
  la.clear();
  BOOST_CHECK(la.empty());
  BOOST_CHECK_EQUAL(la.size(), 0u);
  for (int i = 0; i < N; ++i) BOOST_CHECK(!(*this)[i].hookA.isLinked());
  la.push_back((*this)[3]);
  BOOST_CHECK(ids(la) == (V{3}));
  checkInvariants(la);
}

BOOST_AUTO_TEST_CASE(DestructorUnlinksElements) {
  Node a(1), b(2);
  {
    ListA list;
    list.push_back(a);
    list.push_back(b);
  }
  BOOST_CHECK(!a.hookA.isLinked());
  BOOST_CHECK(!b.hookA.isLinked());
}

// ---------------------------------------------------------------- membership

BOOST_FIXTURE_TEST_CASE(ContainsDistinguishesListsWithSameHook, Fixture) {
  ListA other;
  la.push_back((*this)[0]);
  other.push_back((*this)[1]);
  BOOST_CHECK(la.contains((*this)[0]));
  BOOST_CHECK(!la.contains((*this)[1]));
  BOOST_CHECK(other.contains((*this)[1]));
  BOOST_CHECK(!other.contains((*this)[0]));
  BOOST_CHECK(!la.contains((*this)[2]));
  other.clear();
}

BOOST_FIXTURE_TEST_CASE(ElementInTwoListsViaDifferentHooks, Fixture) {
  for (int i = 0; i < 4; ++i) la.push_back((*this)[i]);
  for (int i = 3; i >= 0; i -= 2) lb.push_back((*this)[i]);
  BOOST_CHECK(ids(la) == (V{0, 1, 2, 3}));
  BOOST_CHECK(ids(lb) == (V{3, 1}));

  la.erase((*this)[1]);                    // removal from A leaves B untouched
  BOOST_CHECK(ids(la) == (V{0, 2, 3}));
  BOOST_CHECK(ids(lb) == (V{3, 1}));
  BOOST_CHECK(lb.contains((*this)[1]));
  BOOST_CHECK(!la.contains((*this)[1]));

  lb.pop_front();                          // removal from B leaves A untouched
  BOOST_CHECK(ids(la) == (V{0, 2, 3}));
  BOOST_CHECK((*this)[3].hookA.isLinked());
  BOOST_CHECK(!(*this)[3].hookB.isLinked());
  checkInvariants(la);
  checkInvariants(lb);
}

// ---------------------------------------------------------------- iterators

BOOST_FIXTURE_TEST_CASE(IteratorIncrementDecrement, Fixture) {
  for (int i = 0; i < 3; ++i) la.push_back((*this)[i]);
  auto it = la.begin();
  BOOST_CHECK_EQUAL((it++)->id, 0);
  BOOST_CHECK_EQUAL(it->id, 1);
  BOOST_CHECK_EQUAL((++it)->id, 2);
  BOOST_CHECK(++it == la.end());
  BOOST_CHECK_EQUAL((--it)->id, 2);       // --end() yields last element
  BOOST_CHECK_EQUAL((it--)->id, 2);
  BOOST_CHECK_EQUAL((*it).id, 1);
  --it;
  BOOST_CHECK(it == la.begin());
}

BOOST_FIXTURE_TEST_CASE(ConstIteration, Fixture) {
  for (int i = 0; i < 3; ++i) la.push_back((*this)[i]);
  const ListA & c = la;
  int sum = 0;
  for (auto it = c.begin(); it != c.end(); ++it) sum += it->id;
  BOOST_CHECK_EQUAL(sum, 3);
  BOOST_CHECK(c.iterator_to((*this)[1]) == std::next(c.begin()));
  ListA::const_iterator conv = la.begin();   // iterator -> const_iterator
  BOOST_CHECK(conv == c.begin());
}

BOOST_FIXTURE_TEST_CASE(ReverseIteration, Fixture) {
  for (int i = 0; i < 4; ++i) la.push_back((*this)[i]);
  BOOST_CHECK(reverseIds(la) == (V{3, 2, 1, 0}));
  auto rit = la.rbegin();
  rit->id = 30;
  BOOST_CHECK_EQUAL(la.back().id, 30);
}

BOOST_FIXTURE_TEST_CASE(StdAlgorithms, Fixture) {
  for (int i = 0; i < N; ++i) la.push_back((*this)[i]);
  BOOST_CHECK_EQUAL(std::distance(la.begin(), la.end()), N);
  auto found = std::find_if(la.begin(), la.end(), [](const Node & n) { return n.id == 5; });
  BOOST_REQUIRE(found != la.end());
  BOOST_CHECK(found == la.iterator_to((*this)[5]));
  BOOST_CHECK_EQUAL(std::count_if(la.cbegin(), la.cend(), [](const Node & n) { return n.id > 3; }), 4);
  for (Node & n : la) n.id *= 10;
  BOOST_CHECK(ids(la) == (V{0, 10, 20, 30, 40, 50, 60, 70}));
}

BOOST_FIXTURE_TEST_CASE(IteratorStableAcrossOtherModifications, Fixture) {
  for (int i = 0; i < 5; ++i) la.push_back((*this)[i]);
  auto it = la.iterator_to((*this)[2]);
  la.erase((*this)[1]);
  la.erase((*this)[3]);
  la.push_front((*this)[5]);
  la.push_back((*this)[6]);
  BOOST_CHECK_EQUAL(it->id, 2);
  BOOST_CHECK_EQUAL(std::next(it)->id, 4);
  BOOST_CHECK_EQUAL(std::prev(it)->id, 0);
}

// ---------------------------------------------------------------- randomized model check

// Random operations mirrored against std::list<int>; fixed seed keeps the test deterministic.
BOOST_AUTO_TEST_CASE(RandomizedAgainstStdList) {
  constexpr int POOL = 64;
  constexpr int OPS  = 20000;
  std::vector<std::unique_ptr<Node>> pool;
  for (int i = 0; i < POOL; ++i) pool.push_back(std::make_unique<Node>(i));

  ListA list;
  std::list<int> model;
  std::mt19937 rng(12345);
  auto pick = [&](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };

  for (int op = 0; op < OPS; ++op) {
    Node & node = *pool[pick(POOL)];
    const bool linked = list.contains(node);
    switch (pick(6)) {
      case 0:                              // push_front
        if (!linked) { list.push_front(node); model.push_front(node.id); }
        break;
      case 1:                              // push_back
        if (!linked) { list.push_back(node); model.push_back(node.id); }
        break;
      case 2:                              // insert before a random element
        if (!linked) {
          const int pos = pick(static_cast<int>(model.size()) + 1);
          list.insert(std::next(list.begin(), pos), node);
          model.insert(std::next(model.begin(), pos), node.id);
        }
        break;
      case 3:                              // erase specific element
        if (linked) {
          list.erase(node);
          model.remove(node.id);
        }
        break;
      case 4:                              // pop front / back
        if (!model.empty()) {
          if (pick(2)) { list.pop_front(); model.pop_front(); }
          else         { list.pop_back();  model.pop_back(); }
        }
        break;
      case 5:                              // occasional clear
        if (pick(50) == 0) { list.clear(); model.clear(); }
        break;
    }
    BOOST_REQUIRE_EQUAL(list.size(), model.size());
    if (op % 256 == 0) {
      BOOST_REQUIRE(ids(list) == V(model.begin(), model.end()));
      checkInvariants(list);
    }
  }
  BOOST_CHECK(ids(list) == V(model.begin(), model.end()));
  checkInvariants(list);
  list.clear();
  for (auto & n : pool) BOOST_CHECK(!n->hookA.isLinked());
}

BOOST_AUTO_TEST_SUITE_END()
