cmake_minimum_required(VERSION 3.25)
project(agentos_superbuild NONE)

option(AGENTOS_STATIC      "Statically link libgcc and libstdc++"  ON)
option(AGENTOS_MUSL        "Build with musl-gcc for full static"   OFF)
option(AGENTOS_BUILD_TESTS "Build unit tests"                      ON)
option(AGENTOS_STRIP       "Strip release binary"                  ON)

set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type")

# Store downloaded dependency sources outside the build directory
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/deps-src")
include(FetchContent)

# spdlog (uses bundled fmt)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.12.0
)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)
if(NOT TARGET spdlog::spdlog)
  message(FATAL_ERROR "Failed to fetch spdlog")
endif()

# libzmq (ZeroMQ core library)
FetchContent_Declare(libzmq
  GIT_REPOSITORY https://github.com/zeromq/libzmq.git
  GIT_TAG v4.3.5
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
FetchContent_MakeAvailable(libzmq)
if(NOT TARGET libzmq-static AND NOT TARGET libzmq)
  message(FATAL_ERROR "Failed to fetch libzmq")
endif()

# cppzmq (C++ binding for ZeroMQ, depends on libzmq)
FetchContent_Declare(cppzmq
  GIT_REPOSITORY https://github.com/zeromq/cppzmq.git
  GIT_TAG v4.10.0
)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPPZMQ_CMAKE_SUPPRESS_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cppzmq)
if(NOT TARGET cppzmq)
  message(FATAL_ERROR "Failed to fetch cppzmq")
endif()
if(TARGET cppzmq AND (TARGET libzmq-static OR TARGET libzmq))
  target_link_libraries(cppzmq INTERFACE libzmq-static)
endif()

# gtest (Google Test)
if(AGENTOS_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
  )
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  if(NOT TARGET GTest::gtest)
    message(FATAL_ERROR "Failed to fetch googletest")
  endif()
endif()

add_subdirectory(src)
