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

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

target_link_libraries(agentos_tests PRIVATE
  agentos_core
  agentos_forge
  ${GTEST_LIB}
  ${GTEST_MAIN_LIB}
  ${ZMQ_LIB}
)

# Provide include paths for headers that are private to agentos_core
target_include_directories(agentos_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src/core
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/include
  ${CMAKE_SOURCE_DIR}/src/core/forge
  ${CPPZMQ_INC}
  ${GTEST_INC}
)

if(AGENTOS_COVERAGE)
  target_compile_options(agentos_tests PRIVATE --coverage -fprofile-arcs -ftest-coverage)
  target_link_options(agentos_tests PRIVATE --coverage)
endif()

# Register with CTest
include(GoogleTest)
gtest_discover_tests(agentos_tests)
if(AGENTOS_BUILD_TESTS)
  enable_testing()

  add_executable(test_types test_types.cpp)
  target_link_libraries(test_types PRIVATE
    ${GTEST_LIB}
    ${GTEST_MAIN_LIB}
    agentos_core
    agentos_forge
  )
  target_include_directories(test_types PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/src/core
    ${GTEST_INC}
  )
  gtest_discover_tests(test_types)
endif()
