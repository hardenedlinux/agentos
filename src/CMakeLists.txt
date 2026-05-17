cmake_minimum_required(VERSION 3.25)
project(agentos VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AGENTOS_STATIC      "Statically link libgcc and libstdc++"  ON)
option(AGENTOS_MUSL        "Build with musl-gcc for full static"   OFF)
option(AGENTOS_BUILD_TESTS "Build unit tests"                      ON)
option(AGENTOS_STRIP       "Strip release binary"                  ON)

find_library(LIBUV_LIB    NAMES uv_a uv     REQUIRED HINTS ${CMAKE_PREFIX_PATH}/lib)
find_path   (LIBUV_INC    uv.h              REQUIRED HINTS ${CMAKE_PREFIX_PATH}/include)
find_path   (RAPIDJSON_INC rapidjson/document.h REQUIRED HINTS ${CMAKE_PREFIX_PATH}/include)
find_path   (SPDLOG_INC   spdlog/spdlog.h   REQUIRED HINTS ${CMAKE_PREFIX_PATH}/include)

message(STATUS "[agentos] libuv     : ${LIBUV_LIB}")
message(STATUS "[agentos] RapidJSON : ${RAPIDJSON_INC}")
message(STATUS "[agentos] spdlog    : ${SPDLOG_INC}")

add_subdirectory(core)
add_subdirectory(cli)

if(AGENTOS_BUILD_TESTS)
  find_library(GTEST_LIB  NAMES gtest      REQUIRED HINTS ${CMAKE_PREFIX_PATH}/lib)
  find_library(GTEST_MAIN NAMES gtest_main REQUIRED HINTS ${CMAKE_PREFIX_PATH}/lib)
  find_library(GMOCK_LIB  NAMES gmock      REQUIRED HINTS ${CMAKE_PREFIX_PATH}/lib)
  find_path   (GTEST_INC  gtest/gtest.h   REQUIRED HINTS ${CMAKE_PREFIX_PATH}/include)
  enable_testing()
  add_subdirectory(tests)
endif()

install(TARGETS agentos RUNTIME DESTINATION bin)
