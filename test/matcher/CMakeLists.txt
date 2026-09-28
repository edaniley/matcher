# test/matcher/CMakeLists.txt
#   MatcherTest   - Boost.Test unit tests for hw::matcher
#   MatcherReplay - JSON Lines replay harness (drives the engine without writing C++)
#   scenarios/    - JSON scenario tests: <name>.input.jsonl replayed and diffed against <name>.expected.jsonl

set(MATCHER_TEST_OPTIONS -Wall -Wextra -fsanitize=address -fno-omit-frame-pointer)

add_executable(MatcherReplay MatcherReplay.cpp)
target_include_directories(MatcherReplay PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(MatcherReplay PRIVATE ${MATCHER_TEST_OPTIONS})
target_link_options(MatcherReplay PRIVATE -fsanitize=address)

add_executable(MatcherTest
    MatcherTest.cpp
    PriceTest.cpp
    OrderBookTest.cpp
    EngineTest.cpp
    JsonReplayTest.cpp
    NoHeapTest.cpp
)
target_include_directories(MatcherTest PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(MatcherTest PRIVATE ${MATCHER_TEST_OPTIONS})
target_link_options(MatcherTest PRIVATE -fsanitize=address)

add_test(NAME MatcherTest COMMAND MatcherTest)

# one CTest entry per scenario file
file(GLOB MATCHER_SCENARIOS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/*.input.jsonl)
foreach(input ${MATCHER_SCENARIOS})
  get_filename_component(file_name ${input} NAME)
  string(REPLACE ".input.jsonl" "" scenario ${file_name})
  add_test(NAME MatcherScenario.${scenario}
    COMMAND ${CMAKE_COMMAND}
      -DREPLAY=$<TARGET_FILE:MatcherReplay>
      -DINPUT=${input}
      -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/scenarios/${scenario}.expected.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/scenarios/${scenario}.actual.jsonl
      -P ${CMAKE_CURRENT_SOURCE_DIR}/RunScenario.cmake)
endforeach()
