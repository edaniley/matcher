#include <boost/test/unit_test.hpp>
#include <hw/utility/SwissTable.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <barrier>
#include <random>

using namespace hw::utility::swisstable;

// Helper to collect errors from threads since Boost macros aren't thread-safe
struct TestMetrics {
    std::atomic<size_t> successes{0};
    std::atomic<size_t> failures{0};
    std::atomic<size_t> retries{0};
};

BOOST_AUTO_TEST_SUITE(SwissTableMTTests)

// 1. Basic Functionality (Single Threaded verification of MT class)
BOOST_AUTO_TEST_CASE(test_mt_basic_ops) {
    Hashmap<int, 64, ThreadSafetyPolicy::Multi> map;
    int val1 = 100;
    int val2 = 200;

    BOOST_CHECK(map.insert(1, &val1));
    BOOST_CHECK(map.insert(2, &val2));
    
    BOOST_CHECK_EQUAL(map.size(), 2);
    BOOST_CHECK_EQUAL(map.find(1), &val1);
    BOOST_CHECK_EQUAL(map.find(2), &val2);
    BOOST_CHECK(map.find(3) == nullptr);

    map.erase(1);
    BOOST_CHECK_EQUAL(map.size(), 1);
    BOOST_CHECK(map.find(1) == nullptr);
    BOOST_CHECK_EQUAL(map.find(2), &val2);
}

// 2. Concurrent Insert (Unique Keys)
// 4 threads inserting disjoint sets of keys
BOOST_AUTO_TEST_CASE(test_mt_concurrent_insert_disjoint) {
    constexpr size_t CAPACITY = 4096; // Must be power of 2
    constexpr size_t THREADS = 4;
    constexpr size_t INSERTS_PER_THREAD = 1000;
    
    Hashmap<int, CAPACITY, ThreadSafetyPolicy::Multi> map;
    std::vector<std::thread> threads;
    std::atomic<bool> error{false};
    int val = 42;

    auto worker = [&](int thread_id) {
        for (size_t i = 0; i < INSERTS_PER_THREAD; ++i) {
            // Interleave keys to encourage collision: thread 0 handles 0, 4, 8...
            uint64_t key = i * THREADS + thread_id; 
            if (!map.insert(key, &val)) {
                error = true;
            }
        }
    };

    for (size_t i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) t.join();

    BOOST_CHECK(!error);
    BOOST_CHECK_EQUAL(map.size(), THREADS * INSERTS_PER_THREAD);
    
    // Verify all keys are present
    for (size_t i = 0; i < THREADS; ++i) {
        for (size_t j = 0; j < INSERTS_PER_THREAD; ++j) {
            uint64_t key = j * THREADS + i;
            if (map.find(key) == nullptr) {
                BOOST_ERROR("Missing key: " << key);
            }
        }
    }
}

// 3. Stress Test: High Contention with 3+ Threads
// Multiple threads hammering the same small set of slots to verify spin/pause logic
BOOST_AUTO_TEST_CASE(test_mt_stress_contention) {
    constexpr size_t CAPACITY = 256; 
    constexpr size_t THREADS = 8;
    constexpr size_t ITERATIONS = 10000;
    
    Hashmap<size_t, CAPACITY, ThreadSafetyPolicy::Multi, DuplicatePolicy::Overwrite> map;
    std::vector<std::thread> threads;
    std::vector<size_t> values(THREADS); // Stable pointers
    
    std::atomic<size_t> total_inserts{0};

    auto worker = [&](size_t id) {
        for (size_t i = 0; i < ITERATIONS; ++i) {
            // Hammer a small range of keys to force collisions and spin-waits
            uint64_t key = i % (CAPACITY / 2); 
            map.insert(key, &values[id]);
            total_inserts++;
        }
    };

    for (size_t i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) t.join();

    BOOST_CHECK(total_inserts == THREADS * ITERATIONS);
    BOOST_CHECK(map.size() <= CAPACITY / 2);
    BOOST_CHECK(map.size() > 0);
}

// 4. Concurrent Insert & Erase
// One group inserts, one group erases
BOOST_AUTO_TEST_CASE(test_mt_insert_erase_race) {
    constexpr size_t CAPACITY = 1024;
    Hashmap<int, CAPACITY, ThreadSafetyPolicy::Multi> map;
    int val = 1;
    std::atomic<bool> running{true};

    std::thread inserter([&]() {
        for (uint64_t i = 0; i < 10000; ++i) {
            map.insert(i % 500, &val); // Loop over 500 keys
        }
    });

    std::thread eraser([&]() {
        for (uint64_t i = 0; i < 10000; ++i) {
            map.erase(i % 500);
        }
    });

    inserter.join();
    eraser.join();
    
    // Just ensuring no deadlock or crash. 
    // State is indeterminate, which is expected.
    BOOST_CHECK(true);
}

// 5. Table Full Scenario
BOOST_AUTO_TEST_CASE(test_mt_full_capacity) {
    constexpr size_t CAPACITY = 128; // Small
    Hashmap<int, CAPACITY, ThreadSafetyPolicy::Multi> map;
    int val = 1;
    
    // Fill it up
    for (uint64_t i = 0; i < CAPACITY; ++i) {
        BOOST_CHECK(map.insert(i, &val));
    }
    
    BOOST_CHECK_EQUAL(map.size(), CAPACITY);
    
    // Try to insert more
    BOOST_CHECK(!map.insert(CAPACITY + 1, &val));
}

BOOST_AUTO_TEST_SUITE_END()
