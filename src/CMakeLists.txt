cmake_minimum_required(VERSION 3.25)
project(agentos VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AGENTOS_STATIC      "Statically link libgcc and libstdc++"  ON)
option(AGENTOS_MUSL        "Build with musl-gcc for full static"   OFF)
option(AGENTOS_BUILD_TESTS "Build unit tests"                      ON)
option(AGENTOS_STRIP       "Strip release binary"                  ON)

# cppzmq (C++ header-only binding)
find_path   (CPPZMQ_INC zmq.hpp              REQUIRED HINTS ${CMAKE_PREFIX_PATH}/include)

# RapidJSON (header-only)
find_path   (RAPIDJSON_INC rapidjson/document.h REQUIRED HINTS ${CMAKE_PREFIX_PATH}/include)

# spdlog (header-only with static library)
find_path   (SPDLOG_INC   spdlog/spdlog.h   REQUIRED HINTS ${CMAKE_PREFIX_PATH}/include)

message(STATUS "[agentos] cppzmq  : ${CPPZMQ_INC}")
message(STATUS "[agentos] RapidJSON : ${RAPIDJSON_INC}")
message(STATUS "[agentos] spdlog    : ${SPDLOG_INC}")

if(AGENTOS_STATIC)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
endif()

add_subdirectory(core)
add_subdirectory(cli)

# Link dependencies using imported targets from FetchContent
target_link_libraries(agentos PRIVATE spdlog::spdlog libzmq cppzmq::cppzmq)

if(AGENTOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)

  # Link test dependencies
  target_link_libraries(agentos_tests PRIVATE spdlog::spdlog libzmq cppzmq::cppzmq GTest::gtest GTest::gtest_main GTest::gmock)
endif()

install(TARGETS agentos RUNTIME DESTINATION bin)
