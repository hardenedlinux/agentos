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
find_library(ZMQ_LIB NAMES zmq libzmq PATHS ${CMAKE_BINARY_DIR}/_deps/libzmq-build/lib NO_DEFAULT_PATH)
if(NOT ZMQ_LIB)
  message(FATAL_ERROR "Could not find libzmq.a in build tree")
endif()

# cppzmq (C++ binding for ZeroMQ, depends on libzmq)
FetchContent_Declare(cppzmq
  GIT_REPOSITORY https://github.com/zeromq/cppzmq.git
  GIT_TAG v4.10.0
)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPPZMQ_CMAKE_SUPPRESS_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cppzmq)
if(TARGET cppzmq)
  set_target_properties(cppzmq PROPERTIES INTERFACE_LINK_LIBRARIES "${ZMQ_LIB}")
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
endif()

add_subdirectory(src)
