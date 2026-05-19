# Unit tests for AgentOS core library
# Built only when AGENTOS_BUILD_TESTS is ON

# Google Test is already fetched by the superbuild (FetchContent_MakeAvailable)
# so the targets GTest::gtest and GTest::gtest_main are available directly.

# Collect all test source files recursively
file(GLOB_RECURSE TEST_SOURCES
  ${CMAKE_SOURCE_DIR}/tests/*.cpp
)

# Create a single test executable containing all tests
add_executable(agentos_tests ${TEST_SOURCES})

target_link_libraries(agentos_tests PRIVATE
  agentos_core
  GTest::gtest
  GTest::gtest_main
)

# Provide include paths for headers that are private to agentos_core
target_include_directories(agentos_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src/core
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/include
)

# Register with CTest
include(GoogleTest)
gtest_discover_tests(agentos_tests)
