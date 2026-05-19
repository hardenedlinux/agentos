cmake_minimum_required(VERSION 3.25)
project(agentos_superbuild NONE)

# Use all available CPU cores for parallel builds
if(NOT DEFINED CMAKE_BUILD_PARALLEL_LEVEL)
  include(ProcessorCount)
  ProcessorCount(N)
  if(N GREATER 0)
    set(CMAKE_BUILD_PARALLEL_LEVEL ${N})
  endif()
endif()

option(AGENTOS_STATIC      "Statically link libgcc and libstdc++"  ON)
option(AGENTOS_MUSL        "Build with musl-gcc for full static"   OFF)
option(AGENTOS_BUILD_TESTS "Build unit tests"                      OFF)
option(AGENTOS_STRIP       "Strip release binary"                  ON)
option(AGENTOS_COVERAGE    "Enable code coverage (gcov/lcov)"      OFF)

set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type")

# Store downloaded dependency sources outside the build directory
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/deps-src")
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# spdlog (uses bundled fmt)
FetchContent_Declare(spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.12.0.tar.gz
)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
message(STATUS "Downloading spdlog...")
FetchContent_MakeAvailable(spdlog)
if(NOT TARGET spdlog::spdlog)
  message(FATAL_ERROR "Failed to fetch spdlog")
endif()

# libzmq (ZeroMQ core library)
FetchContent_Declare(libzmq
  URL https://github.com/zeromq/libzmq/archive/refs/tags/v4.3.5.tar.gz
)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
set(WITH_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURVE OFF CACHE BOOL "" FORCE)
set(ENABLE_PGM OFF CACHE BOOL "" FORCE)
set(ENABLE_NORM OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)
set(ENABLE_DRAFTS OFF CACHE BOOL "" FORCE)
message(STATUS "Downloading libzmq...")
FetchContent_MakeAvailable(libzmq)
if(NOT TARGET libzmq-static AND NOT TARGET libzmq)
  message(FATAL_ERROR "Failed to fetch libzmq")
endif()
find_library(ZMQ_LIB NAMES zmq libzmq PATHS ${FETCHCONTENT_BASE_DIR}/libzmq-build/lib NO_DEFAULT_PATH)
if(NOT ZMQ_LIB)
  message(FATAL_ERROR "Could not find libzmq.a in build tree")
endif()

# SQLite3 (amalgamation) — compile sqlite3.c directly into agentos_core
FetchContent_Declare(sqlite3_amalgamation
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3460000.zip
)
message(STATUS "Downloading sqlite3 amalgamation...")
FetchContent_MakeAvailable(sqlite3_amalgamation)
if(NOT sqlite3_amalgamation_POPULATED)
  message(FATAL_ERROR "Failed to fetch sqlite3 amalgamation")
endif()
set(SQLITE3_SRC_DIR "${sqlite3_amalgamation_SOURCE_DIR}" CACHE INTERNAL "sqlite3 source directory")

# cppzmq (C++ binding for ZeroMQ, header-only, no build needed)
FetchContent_Declare(cppzmq
  URL https://github.com/zeromq/cppzmq/archive/refs/tags/v4.10.0.tar.gz
)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPPZMQ_CMAKE_SUPPRESS_INSTALL OFF CACHE BOOL "" FORCE)
message(STATUS "Downloading cppzmq...")
FetchContent_GetProperties(cppzmq)
if(NOT cppzmq_POPULATED)
  FetchContent_Populate(cppzmq)
endif()
set(CPPZMQ_INC "${cppzmq_SOURCE_DIR}" CACHE INTERNAL "cppzmq include path")

# gtest (Google Test)
if(AGENTOS_BUILD_TESTS)
  enable_testing()
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  )
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST ON CACHE BOOL "" FORCE)
  message(STATUS "Downloading googletest...")
  FetchContent_MakeAvailable(googletest)
  if(NOT TARGET GTest::gtest)
    message(FATAL_ERROR "Failed to fetch googletest")
  endif()
endif()

# Code coverage
if(AGENTOS_COVERAGE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -fprofile-arcs -ftest-coverage")
  set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   --coverage -fprofile-arcs -ftest-coverage")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")

  find_program(LCOV lcov REQUIRED)
  find_program(GENHTML genhtml REQUIRED)
  add_custom_target(coverage
    COMMAND ${CMAKE_COMMAND} -E echo "Running tests for coverage..."
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    COMMAND ${LCOV} --capture --directory ${CMAKE_BINARY_DIR} --output-file ${CMAKE_BINARY_DIR}/coverage.info
    COMMAND ${LCOV} --remove ${CMAKE_BINARY_DIR}/coverage.info '/usr/*' '*/deps-src/*' '*/googletest*' '*/test*' --output-file ${CMAKE_BINARY_DIR}/coverage_filtered.info
    COMMAND ${GENHTML} ${CMAKE_BINARY_DIR}/coverage_filtered.info --output-directory ${CMAKE_BINARY_DIR}/coverage_report
    COMMAND ${CMAKE_COMMAND} -E echo "Coverage report generated at ${CMAKE_BINARY_DIR}/coverage_report/index.html"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating code coverage report"
  )
endif()

add_subdirectory(src)
