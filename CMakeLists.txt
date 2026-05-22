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
# Store ExternalProject stamp and download files outside the build directory
set(CMAKE_EP_STAMP_DIR "${DEPS_BUILD_DIR}/stamps")
set(CMAKE_EP_DOWNLOAD_DIR "${CMAKE_SOURCE_DIR}/deps-src/downloads")

include(ExternalProject)
include(FetchContent)

# spdlog (uses bundled fmt)
set(SPDLOG_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/spdlog")
set(SPDLOG_SRC_DIR "${CMAKE_SOURCE_DIR}/deps-src/spdlog-src")
set(SPDLOG_TARBALL "${CMAKE_SOURCE_DIR}/deps-src/downloads/spdlog-v1.12.0.tar.gz")
if(EXISTS "${SPDLOG_TARBALL}")
  set(SPDLOG_DOWNLOAD_ARGS DOWNLOAD_COMMAND "")
else()
  set(SPDLOG_DOWNLOAD_ARGS "")
endif()
ExternalProject_Add(spdlog_ext
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.12.0.tar.gz
  DOWNLOAD_NAME "spdlog-v1.12.0.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/spdlog
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR ${SPDLOG_SRC_DIR}
  ${SPDLOG_DOWNLOAD_ARGS}
  SOURCE_SUBDIR "spdlog-1.12.0"
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DSPDLOG_BUILD_SHARED=OFF
    -DSPDLOG_BUILD_EXAMPLE=OFF
    -DSPDLOG_BUILD_TESTS=OFF
    -DSPDLOG_FMT_EXTERNAL=OFF
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libspdlog.a
  LOG_DOWNLOAD ON
)
ExternalProject_Get_Property(spdlog_ext INSTALL_DIR)
set(SPDLOG_INC "${INSTALL_DIR}/include" CACHE INTERNAL "spdlog include path")
set(SPDLOG_LIB "${INSTALL_DIR}/lib/libspdlog.a" CACHE INTERNAL "spdlog static library")

# cpp-httplib (header-only HTTP client)
set(HTTPLIB_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/httplib")
set(HTTPLIB_SRC_DIR "${CMAKE_SOURCE_DIR}/deps-src/httplib-src")
set(HTTPLIB_TARBALL "${CMAKE_SOURCE_DIR}/deps-src/downloads/httplib-v0.18.0.tar.gz")
if(EXISTS "${HTTPLIB_TARBALL}")
  set(HTTPLIB_DOWNLOAD_ARGS DOWNLOAD_COMMAND "")
else()
  set(HTTPLIB_DOWNLOAD_ARGS "")
endif()
ExternalProject_Add(httplib_ext
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.0.tar.gz
  DOWNLOAD_NAME "httplib-v0.18.0.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/httplib
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR ${HTTPLIB_SRC_DIR}
  ${HTTPLIB_DOWNLOAD_ARGS}
  SOURCE_SUBDIR "cpp-httplib-0.18.0"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  LOG_DOWNLOAD ON
)
set(HTTPLIB_INC "${CMAKE_SOURCE_DIR}/deps-src/httplib-src" CACHE INTERNAL "cpp-httplib include path")

# rapidjson (header-only JSON library)
set(RAPIDJSON_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/rapidjson")
set(RAPIDJSON_SRC_DIR "${CMAKE_SOURCE_DIR}/deps-src/rapidjson-src")
set(RAPIDJSON_TARBALL "${CMAKE_SOURCE_DIR}/deps-src/downloads/rapidjson-v1.1.0.tar.gz")
if(EXISTS "${RAPIDJSON_TARBALL}")
  set(RAPIDJSON_DOWNLOAD_ARGS DOWNLOAD_COMMAND "")
else()
  set(RAPIDJSON_DOWNLOAD_ARGS "")
endif()
ExternalProject_Add(rapidjson_ext
  URL https://github.com/Tencent/rapidjson/archive/refs/tags/v1.1.0.tar.gz
  DOWNLOAD_NAME "rapidjson-v1.1.0.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/rapidjson
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR ${RAPIDJSON_SRC_DIR}
  ${RAPIDJSON_DOWNLOAD_ARGS}
  SOURCE_SUBDIR "rapidjson-1.1.0"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  LOG_DOWNLOAD ON
)
set(RAPIDJSON_INC "${CMAKE_SOURCE_DIR}/deps-src/rapidjson-src/include" CACHE INTERNAL "rapidjson include path")

# libzmq (ZeroMQ core library)
set(LIBZMQ_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/libzmq")
set(LIBZMQ_SRC_DIR "${CMAKE_SOURCE_DIR}/deps-src/libzmq-src")
set(LIBZMQ_TARBALL "${CMAKE_SOURCE_DIR}/deps-src/downloads/libzmq-v4.3.5.tar.gz")
if(EXISTS "${LIBZMQ_TARBALL}")
  set(LIBZMQ_DOWNLOAD_ARGS DOWNLOAD_COMMAND "")
else()
  set(LIBZMQ_DOWNLOAD_ARGS "")
endif()
ExternalProject_Add(libzmq_ext
  URL https://github.com/zeromq/libzmq/archive/refs/tags/v4.3.5.tar.gz
  DOWNLOAD_NAME "libzmq-v4.3.5.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/libzmq
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR ${LIBZMQ_SRC_DIR}
  ${LIBZMQ_DOWNLOAD_ARGS}
  SOURCE_SUBDIR "libzmq-4.3.5"
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
)
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
set(CPPZMQ_SRC_DIR "${CMAKE_SOURCE_DIR}/deps-src/cppzmq-src")
set(CPPZMQ_TARBALL "${CMAKE_SOURCE_DIR}/deps-src/downloads/cppzmq-v4.10.0.tar.gz")
if(EXISTS "${CPPZMQ_TARBALL}")
  set(CPPZMQ_DOWNLOAD_ARGS DOWNLOAD_COMMAND "")
else()
  set(CPPZMQ_DOWNLOAD_ARGS "")
endif()
ExternalProject_Add(cppzmq_ext
  URL https://github.com/zeromq/cppzmq/archive/refs/tags/v4.10.0.tar.gz
  DOWNLOAD_NAME "cppzmq-v4.10.0.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/cppzmq
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR ${CPPZMQ_SRC_DIR}
  ${CPPZMQ_DOWNLOAD_ARGS}
  SOURCE_SUBDIR "cppzmq-4.10.0"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  LOG_DOWNLOAD ON
)
set(CPPZMQ_INC "${CMAKE_SOURCE_DIR}/deps-src/cppzmq-src" CACHE INTERNAL "cppzmq include path")

# libseccomp (static)
set(SECCOMP_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/libseccomp")
set(SECCOMP_SRC_DIR "${CMAKE_SOURCE_DIR}/deps-src/libseccomp-src")
set(SECCOMP_TARBALL "${CMAKE_SOURCE_DIR}/deps-src/downloads/libseccomp-v2.5.5.tar.gz")
if(EXISTS "${SECCOMP_TARBALL}")
  set(SECCOMP_DOWNLOAD_ARGS DOWNLOAD_COMMAND "")
else()
  set(SECCOMP_DOWNLOAD_ARGS "")
endif()
ExternalProject_Add(libseccomp_ext
  URL https://github.com/seccomp/libseccomp/archive/refs/tags/v2.5.5.tar.gz
  DOWNLOAD_NAME "libseccomp-v2.5.5.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/libseccomp
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR ${SECCOMP_SRC_DIR}
  ${SECCOMP_DOWNLOAD_ARGS}
  SOURCE_SUBDIR "libseccomp-2.5.5"
  PATCH_COMMAND cd <SOURCE_DIR> && autoreconf -iv
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --enable-static --disable-shared
  BUILD_COMMAND make
  INSTALL_COMMAND make install
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libseccomp.a
  LOG_DOWNLOAD ON
)
ExternalProject_Get_Property(libseccomp_ext INSTALL_DIR)
set(SECCOMP_INSTALL_DIR "${INSTALL_DIR}" CACHE INTERNAL "libseccomp install dir")
set(SECCOMP_LIB "${SECCOMP_INSTALL_DIR}/lib/libseccomp.a" CACHE INTERNAL "libseccomp static library")
set(SECCOMP_INCLUDE_DIR "${SECCOMP_INSTALL_DIR}/include" CACHE INTERNAL "libseccomp include dir")

# libcap (static)
set(CAP_PREFIX "${CMAKE_SOURCE_DIR}/deps-src/libcap")
set(CAP_SRC_DIR "${CMAKE_SOURCE_DIR}/deps-src/libcap-src")
set(CAP_TARBALL "${CMAKE_SOURCE_DIR}/deps-src/downloads/libcap-v2.69.tar.gz")
if(EXISTS "${CAP_TARBALL}")
  set(CAP_DOWNLOAD_ARGS DOWNLOAD_COMMAND "")
else()
  set(CAP_DOWNLOAD_ARGS "")
endif()
ExternalProject_Add(libcap_ext
  URL https://git.kernel.org/pub/scm/libs/libcap/libcap.git/snapshot/libcap-2.69.tar.gz
  DOWNLOAD_NAME "libcap-v2.69.tar.gz"
  PREFIX ${DEPS_BUILD_DIR}/libcap
  DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR ${CAP_SRC_DIR}
  ${CAP_DOWNLOAD_ARGS}
  SOURCE_SUBDIR "libcap-2.69"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND make -C <SOURCE_DIR>
  INSTALL_COMMAND make -C <SOURCE_DIR> install RAISE_SETFCAP=no lib=/lib prefix=<INSTALL_DIR>
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libcap.a
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
  set(GOOGLETEST_SRC_DIR "${CMAKE_SOURCE_DIR}/deps-src/googletest-src")
  set(GOOGLETEST_TARBALL "${CMAKE_SOURCE_DIR}/deps-src/downloads/googletest-v1.14.0.tar.gz")
  if(EXISTS "${GOOGLETEST_TARBALL}")
    set(GOOGLETEST_DOWNLOAD_ARGS DOWNLOAD_COMMAND "")
  else()
    set(GOOGLETEST_DOWNLOAD_ARGS "")
  endif()
  ExternalProject_Add(googletest_ext
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    DOWNLOAD_NAME "googletest-v1.14.0.tar.gz"
    PREFIX ${DEPS_BUILD_DIR}/googletest
    DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/deps-src/downloads
    SOURCE_DIR ${GOOGLETEST_SRC_DIR}
    ${GOOGLETEST_DOWNLOAD_ARGS}
    SOURCE_SUBDIR "googletest-1.14.0"
    CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DBUILD_SHARED_LIBS=OFF
      -DINSTALL_GTEST=ON
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libgtest.a
    LOG_DOWNLOAD ON
  )
  ExternalProject_Get_Property(googletest_ext INSTALL_DIR)
  set(GTEST_INC "${INSTALL_DIR}/include" CACHE INTERNAL "googletest include path")
  set(GTEST_LIB "${INSTALL_DIR}/lib/libgtest.a" CACHE INTERNAL "googletest static library")
endif()


add_subdirectory(src)

if(AGENTOS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
