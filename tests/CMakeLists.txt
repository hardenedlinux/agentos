# Unit tests for AgentOS core library
# Built only when AGENTOS_BUILD_TESTS is ON

# Google Test is already fetched by the superbuild (FetchContent_MakeAvailable)
# so the targets GTest::gtest and GTest::gtest_main are available directly.

# Test executable
add_executable(test_database
  ${CMAKE_SOURCE_DIR}/tests/test_database.cpp
)

target_link_libraries(test_database PRIVATE
  agentos_core
  GTest::gtest
  GTest::gtest_main
)

# Provide include paths for headers that are private to agentos_core
target_include_directories(test_database PRIVATE
  ${CMAKE_SOURCE_DIR}/src/core
  ${CMAKE_SOURCE_DIR}/include
)

# Register with CTest
include(GoogleTest)
gtest_discover_tests(test_database)
