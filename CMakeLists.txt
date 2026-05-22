cmake_minimum_required(VERSION 3.25)
project(agentos_superbuild C CXX)

set(CMAKE_CXX_STANDARD 17)
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

# Store downloaded dependency sources outside the build directory
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/deps-src")
set(DEPS_BUILD_DIR "${CMAKE_SOURCE_DIR}/deps-build")
# Place build artifacts in deps-build/build (run cmake with -B deps-build)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
# Store ExternalProject stamp and download files outside the build directory
set(CMAKE_EP_STAMP_DIR "${DEPS_BUILD_DIR}/stamps")
set(CMAKE_EP_DOWNLOAD_DIR "${CMAKE_SOURCE_DIR}/deps-src/downloads")

include(ExternalProject)
include(FetchContent)

# spdlog (uses bundled fmt)
set(SPDLOG_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/spdlog")
set(CMAKE_EP_BUILD_DIR "${DEPS_BUILD_DIR}/spdlog/build")
ExternalProject_Add(spdlog_ext
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.12.0.tar.gz
  DOWNLOAD_NAME "spdlog-v1.12.0.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/spdlog
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DSPDLOG_BUILD_SHARED=OFF
    -DSPDLOG_BUILD_EXAMPLE=OFF
    -DSPDLOG_BUILD_TESTS=OFF
    -DSPDLOG_FMT_EXTERNAL=OFF
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libspdlog.a
  LOG_DOWNLOAD ON
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
  COMMAND ${CMAKE_COMMAND} -E echo "{\"name\":\"spdlog\",\"version\":\"1.12.0\"}" > ${DEPS_BUILD_DIR}/spdlog/Labels.json
  COMMAND ${CMAKE_COMMAND} -E echo "spdlog 1.12.0" > ${DEPS_BUILD_DIR}/spdlog/Labels.txt
)
unset(CMAKE_EP_BUILD_DIR)
ExternalProject_Get_Property(spdlog_ext INSTALL_DIR)
set(SPDLOG_INC "${INSTALL_DIR}/include" CACHE INTERNAL "spdlog include path")
set(SPDLOG_LIB "${INSTALL_DIR}/lib/libspdlog.a" CACHE INTERNAL "spdlog static library")

# cpp-httplib (header-only HTTP client)
set(HTTPLIB_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/httplib")
ExternalProject_Add(httplib_ext
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.0.tar.gz
  DOWNLOAD_NAME "httplib-v0.18.0.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/httplib
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  LOG_DOWNLOAD ON
  BUILD_IN_SOURCE TRUE
  BUILD_COMMAND ${CMAKE_COMMAND} -E echo "{\"name\":\"cpp-httplib\",\"version\":\"0.18.0\"}" > ${DEPS_BUILD_DIR}/httplib/Labels.json
  COMMAND ${CMAKE_COMMAND} -E echo "cpp-httplib 0.18.0" > ${DEPS_BUILD_DIR}/httplib/Labels.txt
)
set(HTTPLIB_INC "${DEPS_BUILD_DIR}/httplib/src/httplib_ext/cpp-httplib-0.18.0" CACHE INTERNAL "cpp-httplib include path")

# rapidjson (header-only JSON library)
set(RAPIDJSON_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/rapidjson")
ExternalProject_Add(rapidjson_ext
  URL https://github.com/Tencent/rapidjson/archive/refs/tags/v1.1.0.tar.gz
  DOWNLOAD_NAME "rapidjson-v1.1.0.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/rapidjson
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  LOG_DOWNLOAD ON
  BUILD_IN_SOURCE TRUE
  BUILD_COMMAND ${CMAKE_COMMAND} -E echo "{\"name\":\"rapidjson\",\"version\":\"1.1.0\"}" > ${DEPS_BUILD_DIR}/rapidjson/Labels.json
  COMMAND ${CMAKE_COMMAND} -E echo "rapidjson 1.1.0" > ${DEPS_BUILD_DIR}/rapidjson/Labels.txt
)
set(RAPIDJSON_INC "${DEPS_BUILD_DIR}/rapidjson/src/rapidjson_ext/rapidjson-1.1.0/include" CACHE INTERNAL "rapidjson include path")

# libzmq (ZeroMQ core library)
set(LIBZMQ_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/libzmq")
set(CMAKE_EP_BUILD_DIR "${DEPS_BUILD_DIR}/libzmq/build")
ExternalProject_Add(libzmq_ext
  URL https://github.com/zeromq/libzmq/archive/refs/tags/v4.3.5.tar.gz
  DOWNLOAD_NAME "libzmq-v4.3.5.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/libzmq
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DBUILD_SHARED=OFF
    -DBUILD_STATIC=ON
    -DZMQ_BUILD_TESTS=OFF
    -DWITH_PERF_TOOL=OFF
    -DWITH_DOCS=OFF
    -DENABLE_CURVE=OFF
    -DENABLE_PGM=OFF
    -DENABLE_NORM=OFF
    -DENABLE_GSSAPI=OFF
    -DENABLE_DRAFTS=OFF
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libzmq.a
  LOG_DOWNLOAD ON
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
  COMMAND ${CMAKE_COMMAND} -E echo "{\"name\":\"libzmq\",\"version\":\"4.3.5\"}" > ${DEPS_BUILD_DIR}/libzmq/Labels.json
  COMMAND ${CMAKE_COMMAND} -E echo "libzmq 4.3.5" > ${DEPS_BUILD_DIR}/libzmq/Labels.txt
)
unset(CMAKE_EP_BUILD_DIR)
ExternalProject_Get_Property(libzmq_ext INSTALL_DIR)
set(ZMQ_LIB "${INSTALL_DIR}/lib/libzmq.a" CACHE INTERNAL "libzmq static library")

# SQLite3 (amalgamation) — compile sqlite3.c directly into agentos_core
FetchContent_Declare(sqlite3_amalgamation
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3460000.zip
  SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps-src/sqlite3_amalgamation-src"
)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
message(STATUS "Downloading sqlite3 amalgamation...")
FetchContent_MakeAvailable(sqlite3_amalgamation)
if(NOT sqlite3_amalgamation_POPULATED)
  message(FATAL_ERROR "Failed to fetch sqlite3 amalgamation")
endif()
set(SQLITE3_SRC_DIR "${sqlite3_amalgamation_SOURCE_DIR}" CACHE INTERNAL "sqlite3 source directory")

# cppzmq (C++ binding for ZeroMQ, header-only, no build needed)
set(CPPZMQ_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/cppzmq")
ExternalProject_Add(cppzmq_ext
  URL https://github.com/zeromq/cppzmq/archive/refs/tags/v4.10.0.tar.gz
  DOWNLOAD_NAME "cppzmq-v4.10.0.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/cppzmq
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  LOG_DOWNLOAD ON
  BUILD_IN_SOURCE TRUE
  BUILD_COMMAND ${CMAKE_COMMAND} -E echo "{\"name\":\"cppzmq\",\"version\":\"4.10.0\"}" > ${DEPS_BUILD_DIR}/cppzmq/Labels.json
  COMMAND ${CMAKE_COMMAND} -E echo "cppzmq 4.10.0" > ${DEPS_BUILD_DIR}/cppzmq/Labels.txt
)
set(CPPZMQ_INC "${DEPS_BUILD_DIR}/cppzmq/src/cppzmq_ext/cppzmq-4.10.0" CACHE INTERNAL "cppzmq include path")

# libseccomp (static)
set(SECCOMP_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/libseccomp")
ExternalProject_Add(libseccomp_ext
  URL https://github.com/seccomp/libseccomp/archive/refs/tags/v2.5.5.tar.gz
  DOWNLOAD_NAME "libseccomp-v2.5.5.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/libseccomp
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  PATCH_COMMAND cd <SOURCE_DIR> && autoreconf -iv
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --enable-static --disable-shared
  BUILD_COMMAND make
  COMMAND ${CMAKE_COMMAND} -E echo "{\"name\":\"libseccomp\",\"version\":\"2.5.5\"}" > ${DEPS_BUILD_DIR}/libseccomp/Labels.json
  COMMAND ${CMAKE_COMMAND} -E echo "libseccomp 2.5.5" > ${DEPS_BUILD_DIR}/libseccomp/Labels.txt
  INSTALL_COMMAND make install
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libseccomp.a
  LOG_DOWNLOAD ON
  BUILD_IN_SOURCE TRUE
)
ExternalProject_Get_Property(libseccomp_ext INSTALL_DIR)
set(SECCOMP_INSTALL_DIR "${INSTALL_DIR}" CACHE INTERNAL "libseccomp install dir")
set(SECCOMP_LIB "${SECCOMP_INSTALL_DIR}/lib/libseccomp.a" CACHE INTERNAL "libseccomp static library")
set(SECCOMP_INCLUDE_DIR "${SECCOMP_INSTALL_DIR}/include" CACHE INTERNAL "libseccomp include dir")

# libcap (static)
set(CAP_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/libcap")
ExternalProject_Add(libcap_ext
  URL https://git.kernel.org/pub/scm/libs/libcap/libcap.git/snapshot/libcap-2.69.tar.gz
  DOWNLOAD_NAME "libcap-v2.69.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/libcap
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  CONFIGURE_COMMAND ""
  BUILD_COMMAND make -C <SOURCE_DIR>/libcap SHARED=no
  COMMAND ${CMAKE_COMMAND} -E echo "{\"name\":\"libcap\",\"version\":\"2.69\"}" > ${DEPS_BUILD_DIR}/libcap/Labels.json
  COMMAND ${CMAKE_COMMAND} -E echo "libcap 2.69" > ${DEPS_BUILD_DIR}/libcap/Labels.txt
  INSTALL_COMMAND make -C <SOURCE_DIR>/libcap SHARED=no install RAISE_SETFCAP=no prefix=<INSTALL_DIR> DESTDIR=
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libcap.a
  BUILD_IN_SOURCE TRUE
  STAMP_DIR ${DEPS_BUILD_DIR}/stamps/libcap
)
ExternalProject_Get_Property(libcap_ext INSTALL_DIR)
set(CAP_INSTALL_DIR "${INSTALL_DIR}" CACHE INTERNAL "libcap install dir")
set(CAP_LIB "${CAP_INSTALL_DIR}/lib/libcap.a" CACHE INTERNAL "libcap static library")
set(CAP_INCLUDE_DIR "${CAP_INSTALL_DIR}/include" CACHE INTERNAL "libcap include dir")

# OpenSSL (for HTTPS support in cpp-httplib)
set(OPENSSL_USE_STATIC_LIBS ON)
find_package(OpenSSL REQUIRED)

# gtest (Google Test)
if(AGENTOS_BUILD_TESTS)
  enable_testing()
  set(GOOGLETEST_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/googletest")
  set(CMAKE_EP_BUILD_DIR "${DEPS_BUILD_DIR}/googletest/build")
  ExternalProject_Add(googletest_ext
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    DOWNLOAD_NAME "googletest-v1.14.0.tar.gz"
    PREFIX ${DEPS_BUILD_DIR}/googletest
    DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
    CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DBUILD_SHARED_LIBS=OFF
      -DINSTALL_GTEST=ON
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libgtest.a
    LOG_DOWNLOAD ON
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
    COMMAND ${CMAKE_COMMAND} -E echo "{\"name\":\"googletest\",\"version\":\"1.14.0\"}" > ${DEPS_BUILD_DIR}/googletest/Labels.json
    COMMAND ${CMAKE_COMMAND} -E echo "googletest 1.14.0" > ${DEPS_BUILD_DIR}/googletest/Labels.txt
  )
  unset(CMAKE_EP_BUILD_DIR)
  ExternalProject_Get_Property(googletest_ext INSTALL_DIR)
  set(GTEST_INC "${INSTALL_DIR}/include" CACHE INTERNAL "googletest include path")
  set(GTEST_LIB "${INSTALL_DIR}/lib/libgtest.a" CACHE INTERNAL "googletest static library")
endif()


add_subdirectory(src)

if(AGENTOS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
