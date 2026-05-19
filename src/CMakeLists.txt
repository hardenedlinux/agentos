cmake_minimum_required(VERSION 3.25)
project(agentos VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AGENTOS_STATIC      "Statically link libgcc and libstdc++"  ON)
option(AGENTOS_MUSL        "Build with musl-gcc for full static"   OFF)
option(AGENTOS_BUILD_TESTS "Build unit tests"                      ON)
option(AGENTOS_STRIP       "Strip release binary"                  ON)

# cppzmq (C++ header-only binding)
if(NOT DEFINED CPPZMQ_INC)
  find_path(CPPZMQ_INC zmq.hpp REQUIRED HINTS ${CMAKE_PREFIX_PATH}/include)
endif()

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

# Add project include directory
include_directories(BEFORE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_subdirectory(core)
add_subdirectory(cli)

# Link dependencies using imported targets from FetchContent
target_link_libraries(agentos PRIVATE spdlog::spdlog ${ZMQ_LIB})

if(AGENTOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)

  # Link test dependencies
  target_link_libraries(agentos_tests PRIVATE spdlog::spdlog ${ZMQ_LIB} GTest::gtest GTest::gtest_main GTest::gmock)
endif()

install(TARGETS agentos RUNTIME DESTINATION dist/bin)

# Copy binary to expected location for build script
set(EXPECTED_BIN_DIR "${CMAKE_BINARY_DIR}/dist/bin")
file(MAKE_DIRECTORY "${EXPECTED_BIN_DIR}")
add_custom_target(copy_agentos ALL
  DEPENDS agentos
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:agentos> "${EXPECTED_BIN_DIR}/agentos"
  COMMENT "Copying agentos to ${EXPECTED_BIN_DIR}/agentos"
)
