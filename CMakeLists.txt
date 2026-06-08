# Copyright (C) 2026  HardenedLinux community
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.25)
project(agentos_superbuild C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AGENTOS_STATIC      "Statically link libgcc and libstdc++"  ON)
option(AGENTOS_MUSL        "Build with musl-gcc for full static"   OFF)
option(AGENTOS_BUILD_TESTS "Build unit tests"                      OFF)
option(AGENTOS_STRIP       "Strip release binary"                  ON)
option(AGENTOS_COVERAGE    "Enable code coverage (gcov/lcov)"      OFF)

if(AGENTOS_STATIC)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
endif()

# Use all available CPU cores for parallel builds
if(NOT DEFINED CMAKE_BUILD_PARALLEL_LEVEL)
  include(ProcessorCount)
  ProcessorCount(N)
  if(N GREATER 0)
    set(CMAKE_BUILD_PARALLEL_LEVEL ${N})
  endif()
endif()

set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type")

# Dependency source and build directories
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/deps-src")
set(DEPS_BUILD_DIR        "${CMAKE_SOURCE_DIR}/deps-build")
set(DEPS_INSTALL_DIR      "${DEPS_BUILD_DIR}/install" CACHE INTERNAL "deps install root")

# Build output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")

# ExternalProject stamp and download locations
set(CMAKE_EP_STAMP_DIR    "${DEPS_BUILD_DIR}/stamps")
set(CMAKE_EP_DOWNLOAD_DIR "${CMAKE_SOURCE_DIR}/deps-src/downloads")

# All third-party dependencies
add_subdirectory(deps)

# Main project
add_subdirectory(src)

if(AGENTOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
