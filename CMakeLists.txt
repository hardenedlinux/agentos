cmake_minimum_required(VERSION 3.25)
project(agentos_superbuild NONE)

option(AGENTOS_STATIC      "Statically link libgcc and libstdc++"  ON)
option(AGENTOS_MUSL        "Build with musl-gcc for full static"   OFF)
option(AGENTOS_BUILD_TESTS "Build unit tests"                      ON)
option(AGENTOS_STRIP       "Strip release binary"                  ON)

set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type")

# All deps install headers + static libs here.
# src/ build uses this as CMAKE_PREFIX_PATH.
set(DEPS_INSTALL_DIR "${CMAKE_BINARY_DIR}/deps/install"
    CACHE PATH "Where deps install their headers and libs")

  include(ExternalProject)

  # Step 1: deps
  # deps/CMakeLists.txt runs each dep as its own ExternalProject.
  # It is purely a build-time orchestrator — it has no install target itself.
  # We use BUILD_COMMAND to drive it and suppress the default INSTALL_COMMAND.
  ExternalProject_Add(deps
    SOURCE_DIR      "${CMAKE_SOURCE_DIR}/deps"
    BINARY_DIR      "${CMAKE_BINARY_DIR}/deps/build"
    CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DDEPS_INSTALL_DIR=${DEPS_INSTALL_DIR}
    -DBUILD_SHARED_LIBS=OFF
    BUILD_ALWAYS    FALSE
    INSTALL_COMMAND "" # deps installs itself during BUILD via each sub-ExternalProject
  )

  # Step 2: agentos core
  ExternalProject_Add(agentos
    DEPENDS         deps
    SOURCE_DIR      "${CMAKE_SOURCE_DIR}/src"
    BINARY_DIR      "${CMAKE_BINARY_DIR}/src/build"
    INSTALL_DIR     "${CMAKE_BINARY_DIR}/dist"
    CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/dist
    -DCMAKE_PREFIX_PATH=${DEPS_INSTALL_DIR}
    -DAGENTOS_STATIC=${AGENTOS_STATIC}
    -DAGENTOS_MUSL=${AGENTOS_MUSL}
    -DAGENTOS_BUILD_TESTS=${AGENTOS_BUILD_TESTS}
    -DAGENTOS_STRIP=${AGENTOS_STRIP}
    BUILD_ALWAYS    TRUE
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
  )
