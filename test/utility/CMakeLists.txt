# test/utility/CMakeLists.txt
# UtilityTest: Boost.Test (header-only, included variant) unit tests for hw::utility

add_executable(UtilityTest
    UtilityTest.cpp
    DListTest.cpp
    TestAllocator.cpp
    TestHashTableTrivial.cpp
    TestHashArray.cpp
    TestSwissTableMT.cpp
    TestKeyBuilder.cpp
    TestPriorityQueue.cpp
    TestEPoller.cpp
)

target_include_directories(UtilityTest PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(UtilityTest PRIVATE Threads::Threads)

target_compile_options(UtilityTest PRIVATE -Wall -Wextra -fsanitize=address -fno-omit-frame-pointer)
target_link_options(UtilityTest PRIVATE -fsanitize=address)

add_test(NAME UtilityTest COMMAND UtilityTest)
