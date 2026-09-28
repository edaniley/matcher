#include <boost/test/unit_test.hpp>
#include <hw/utility/PriorityQueue.hpp>
#include <functional>

BOOST_AUTO_TEST_SUITE(PriorityQueueTests)

// 1. Basic Functionality
BOOST_AUTO_TEST_CASE(BasicOperations) {
    hw::utility::PriorityQueue<int, 5> pq;
    BOOST_CHECK(pq.empty());
    BOOST_CHECK_EQUAL(pq.size(), 0);

    BOOST_CHECK(pq.push(10));
    BOOST_CHECK(!pq.empty());
    BOOST_CHECK_EQUAL(pq.size(), 1);
    BOOST_CHECK_EQUAL(pq.top(), 10);

    BOOST_CHECK(pq.push(5));
    BOOST_CHECK_EQUAL(pq.size(), 2);
    BOOST_CHECK_EQUAL(pq.top(), 10); // 10 is max

    BOOST_CHECK(pq.push(20));
    BOOST_CHECK_EQUAL(pq.size(), 3);
    BOOST_CHECK_EQUAL(pq.top(), 20); // 20 is max

    pq.pop();
    BOOST_CHECK_EQUAL(pq.size(), 2);
    BOOST_CHECK_EQUAL(pq.top(), 10);

    pq.pop();
    BOOST_CHECK_EQUAL(pq.size(), 1);
    BOOST_CHECK_EQUAL(pq.top(), 5);

    pq.pop();
    BOOST_CHECK(pq.empty());
}

// 2. Overflow Protection
BOOST_AUTO_TEST_CASE(OverflowTest) {
    hw::utility::PriorityQueue<int, 2> pq;
    BOOST_CHECK(pq.push(1));
    BOOST_CHECK(pq.push(2));
    BOOST_CHECK_EQUAL(pq.size(), 2);

    // Attempt to push beyond capacity
    bool result = pq.push(3);
    
    BOOST_CHECK(!result); 
    BOOST_CHECK_EQUAL(pq.size(), 2);
}

// 3. Custom Comparator (Min-Heap)
BOOST_AUTO_TEST_CASE(CustomComparator) {
    // Min-heap using std::greater
    hw::utility::PriorityQueue<int, 5, std::greater<int>> pq;
    pq.push(10);
    pq.push(5);
    pq.push(20);
    
    BOOST_CHECK_EQUAL(pq.top(), 5);
    pq.pop();
    BOOST_CHECK_EQUAL(pq.top(), 10);
    pq.pop();
    BOOST_CHECK_EQUAL(pq.top(), 20);
}

// 4. Move Semantics and Emplace
struct MoveOnly {
    int value;
    MoveOnly() : value(0) {} // Default ctor needed for array
    MoveOnly(int v) : value(v) {}
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    
    bool operator<(const MoveOnly& other) const { return value < other.value; }
};

BOOST_AUTO_TEST_CASE(MoveOnlyTest) {
    hw::utility::PriorityQueue<MoveOnly, 5> pq;
    pq.push(MoveOnly(10));
    pq.emplace(20);
    
    BOOST_CHECK_EQUAL(pq.size(), 2);
    BOOST_CHECK_EQUAL(pq.top().value, 20);
    
    pq.pop();
    BOOST_CHECK_EQUAL(pq.size(), 1);
    BOOST_CHECK_EQUAL(pq.top().value, 10);
}

BOOST_AUTO_TEST_SUITE_END()
