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

# All third-party dependencies.
#
# Deps that require compilation use -DCMAKE_CXX_STANDARD=17 explicitly so
# they are never affected by the parent project's C++23 setting.
# Header-only deps (rapidjson, cppzmq, httplib, tomlplusplus) have no build
# step and are not affected by the standard either way.
# libseccomp and libcap use autoconf/make and are C-only; no CXX standard needed.
#
# Design:
#   URL          = remote address; cmake downloads on first run
#   DOWNLOAD_DIR = deps-src/downloads/; tarballs are cached here permanently
#   URL_HASH     = sha256; if local tarball matches, network is never touched
#   SOURCE_DIR   = fixed path under deps-src/; survives rm -rf build/
#   STAMP_DIR    = fixed path under deps-build/stamps/; survives rm -rf build/
#
# Result: delete build/ as many times as you like — deps are never
# re-downloaded (tarball cache) and never re-built (stamp cache).

include(ExternalProject)
include(FetchContent)

# ---------------------------------------------------------------------------
# spdlog (compiled, static)
# ---------------------------------------------------------------------------
ExternalProject_Add(spdlog_ext
  URL           https://github.com/gabime/spdlog/archive/refs/tags/v1.12.0.tar.gz
  URL_HASH      SHA256=4dccf2d10f410c1e2feaff89966bfc49a1abb29ef6f08246335b110e001e09a9
  DOWNLOAD_NAME spdlog-v1.12.0.tar.gz
  PREFIX        ${DEPS_BUILD_DIR}/spdlog
  DOWNLOAD_DIR  ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR    ${CMAKE_SOURCE_DIR}/deps-src/spdlog-src
  STAMP_DIR     ${DEPS_BUILD_DIR}/stamps/spdlog
  BINARY_DIR    ${DEPS_BUILD_DIR}/spdlog/build
  CMAKE_ARGS
  -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
  -DCMAKE_CXX_STANDARD=17
  -DSPDLOG_BUILD_SHARED=OFF
  -DSPDLOG_BUILD_EXAMPLE=OFF
  -DSPDLOG_BUILD_TESTS=OFF
  -DSPDLOG_FMT_EXTERNAL=OFF
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libspdlog.a
  LOG_DOWNLOAD  ON
  LOG_CONFIGURE ON
  LOG_BUILD     ON
  LOG_OUTPUT_ON_FAILURE ON
)
ExternalProject_Get_Property(spdlog_ext INSTALL_DIR)
set(SPDLOG_INC "${INSTALL_DIR}/include"         CACHE INTERNAL "spdlog include path")
set(SPDLOG_LIB "${INSTALL_DIR}/lib/libspdlog.a" CACHE INTERNAL "spdlog static library")

# ---------------------------------------------------------------------------
# cpp-httplib (header-only)
# ---------------------------------------------------------------------------
ExternalProject_Add(httplib_ext
  URL           https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.0.tar.gz
  URL_HASH      SHA256=6ed5894bbbc4a34a0f4c5e962672d0003d2ea099bbadacc66f6dee2b213ff394
  DOWNLOAD_NAME httplib-v0.18.0.tar.gz
  PREFIX        ${DEPS_BUILD_DIR}/httplib
  DOWNLOAD_DIR  ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR    ${CMAKE_SOURCE_DIR}/deps-src/httplib-src
  STAMP_DIR     ${DEPS_BUILD_DIR}/stamps/httplib
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  LOG_DOWNLOAD ON
  LOG_OUTPUT_ON_FAILURE ON
)
set(HTTPLIB_INC "${CMAKE_SOURCE_DIR}/deps-src/httplib-src"
  CACHE INTERNAL "cpp-httplib include path")

# ---------------------------------------------------------------------------
# RapidJSON (header-only)
# ---------------------------------------------------------------------------
ExternalProject_Add(rapidjson_ext
  URL           https://github.com/Tencent/rapidjson/archive/24b5e7a8b27f42fa16b96fc70aade9106cf7102f.tar.gz
  URL_HASH      SHA256=2d2601a82d2d3b7e143a3c8d43ef616671391034bc46891a9816b79cf2d3e7a8
  DOWNLOAD_NAME rapidjson-24b5e7a.tar.gz
  PREFIX        ${DEPS_BUILD_DIR}/rapidjson
  DOWNLOAD_DIR  ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR    ${CMAKE_SOURCE_DIR}/deps-src/rapidjson-src
  STAMP_DIR     ${DEPS_BUILD_DIR}/stamps/rapidjson
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  LOG_DOWNLOAD ON
  LOG_OUTPUT_ON_FAILURE ON
)
set(RAPIDJSON_INC "${CMAKE_SOURCE_DIR}/deps-src/rapidjson-src/include"
  CACHE INTERNAL "rapidjson include path")

# ---------------------------------------------------------------------------
# libzmq (compiled, static)
# ---------------------------------------------------------------------------
ExternalProject_Add(libzmq_ext
  URL           https://github.com/zeromq/libzmq/archive/refs/tags/v4.3.5.tar.gz
  URL_HASH      SHA256=6c972d1e6a91a0ecd79c3236f04cf0126f2f4dfbbad407d72b4606a7ba93f9c6
  DOWNLOAD_NAME libzmq-v4.3.5.tar.gz
  PREFIX        ${DEPS_BUILD_DIR}/libzmq
  DOWNLOAD_DIR  ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR    ${CMAKE_SOURCE_DIR}/deps-src/libzmq-src
  STAMP_DIR     ${DEPS_BUILD_DIR}/stamps/libzmq
  BINARY_DIR    ${DEPS_BUILD_DIR}/libzmq/build
  CMAKE_ARGS
  -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
  -DCMAKE_CXX_STANDARD=17
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
  LOG_DOWNLOAD  ON
  LOG_CONFIGURE ON
  LOG_BUILD     ON
  LOG_OUTPUT_ON_FAILURE ON
)
ExternalProject_Get_Property(libzmq_ext INSTALL_DIR)
set(ZMQ_LIB "${INSTALL_DIR}/lib/libzmq.a" CACHE INTERNAL "libzmq static library")
set(ZMQ_INC "${INSTALL_DIR}/include"      CACHE INTERNAL "libzmq include path")

# ---------------------------------------------------------------------------
# cppzmq (header-only C++ wrapper for libzmq)
# ---------------------------------------------------------------------------
ExternalProject_Add(cppzmq_ext
  URL           https://github.com/zeromq/cppzmq/archive/refs/tags/v4.10.0.tar.gz
  URL_HASH      SHA256=c81c81bba8a7644c84932225f018b5088743a22999c6d82a2b5f5cd1e6942b74
  DOWNLOAD_NAME cppzmq-v4.10.0.tar.gz
  PREFIX        ${DEPS_BUILD_DIR}/cppzmq
  DOWNLOAD_DIR  ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR    ${CMAKE_SOURCE_DIR}/deps-src/cppzmq-src
  STAMP_DIR     ${DEPS_BUILD_DIR}/stamps/cppzmq
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  LOG_DOWNLOAD ON
  LOG_OUTPUT_ON_FAILURE ON
)
set(CPPZMQ_INC "${CMAKE_SOURCE_DIR}/deps-src/cppzmq-src"
  CACHE INTERNAL "cppzmq include path")

# ---------------------------------------------------------------------------
# toml++ (header-only, single file download)
# ---------------------------------------------------------------------------
set(TOMLPLUSPLUS_DIR "${CMAKE_SOURCE_DIR}/deps-src/tomlplusplus")
if(NOT EXISTS "${TOMLPLUSPLUS_DIR}/toml.hpp")
  file(MAKE_DIRECTORY "${TOMLPLUSPLUS_DIR}")
  file(DOWNLOAD
    https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp
    "${TOMLPLUSPLUS_DIR}/toml.hpp"
    STATUS download_status
    LOG    download_log
  )
  list(GET download_status 0 download_code)
  if(NOT download_code EQUAL 0)
    message(FATAL_ERROR "Failed to download toml.hpp: ${download_log}")
  endif()
endif()
set(TOMLPLUSPLUS_INC "${TOMLPLUSPLUS_DIR}" CACHE INTERNAL "tomlplusplus include path")

# ---------------------------------------------------------------------------
# SQLite3 amalgamation (compiled directly into agentos_core)
# ---------------------------------------------------------------------------
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
FetchContent_Declare(sqlite3_amalgamation
  URL        https://www.sqlite.org/2024/sqlite-amalgamation-3460000.zip
  SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps-src/sqlite3_amalgamation-src"
)
message(STATUS "Fetching sqlite3 amalgamation...")
FetchContent_MakeAvailable(sqlite3_amalgamation)
if(NOT sqlite3_amalgamation_POPULATED)
  message(FATAL_ERROR "Failed to fetch sqlite3 amalgamation")
endif()
set(SQLITE3_SRC_DIR "${sqlite3_amalgamation_SOURCE_DIR}"
  CACHE INTERNAL "sqlite3 source directory")

# ---------------------------------------------------------------------------
# libseccomp (C only, autoconf/make)
# ---------------------------------------------------------------------------
ExternalProject_Add(libseccomp_ext
  URL           https://github.com/seccomp/libseccomp/archive/refs/tags/v2.5.5.tar.gz
  URL_HASH      SHA256=7082b016d3cbda3e15c0e71ebd018023d693bb7507389b32f943db13f935e01d
  DOWNLOAD_NAME libseccomp-v2.5.5.tar.gz
  PREFIX        ${DEPS_BUILD_DIR}/libseccomp
  DOWNLOAD_DIR  ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR    ${CMAKE_SOURCE_DIR}/deps-src/libseccomp-src
  STAMP_DIR     ${DEPS_BUILD_DIR}/stamps/libseccomp
  BINARY_DIR    ${DEPS_BUILD_DIR}/libseccomp/build
  PATCH_COMMAND cd <SOURCE_DIR> && autoreconf -iv
  CONFIGURE_COMMAND
  <SOURCE_DIR>/configure
  --prefix=<INSTALL_DIR>
  --enable-static
  --disable-shared
  BUILD_COMMAND   make
  INSTALL_COMMAND make install
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libseccomp.a
  LOG_DOWNLOAD  ON
  LOG_CONFIGURE ON
  LOG_BUILD     ON
  LOG_OUTPUT_ON_FAILURE ON
)
ExternalProject_Get_Property(libseccomp_ext INSTALL_DIR)
set(SECCOMP_LIB         "${INSTALL_DIR}/lib/libseccomp.a" CACHE INTERNAL "libseccomp static library")
set(SECCOMP_INCLUDE_DIR "${INSTALL_DIR}/include"          CACHE INTERNAL "libseccomp include dir")

# ---------------------------------------------------------------------------
# libcap (C only, make, SHARED=no)
# Note: kernel.org may require a proxy for first download; after the tarball
# is cached in deps-src/downloads/ it is never fetched again.
# ---------------------------------------------------------------------------
ExternalProject_Add(libcap_ext
  URL           https://git.kernel.org/pub/scm/libs/libcap/libcap.git/snapshot/libcap-2.69.tar.gz
  DOWNLOAD_NAME libcap-v2.69.tar.gz
  URL_HASH      SHA256=9cc2fa3ee744d881629cdac1a1b54c302e7684fda3e3622056218c7681642ffb
  PREFIX        ${DEPS_BUILD_DIR}/libcap
  DOWNLOAD_DIR  ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR    ${CMAKE_SOURCE_DIR}/deps-src/libcap-src
  STAMP_DIR     ${DEPS_BUILD_DIR}/stamps/libcap
  CONFIGURE_COMMAND ""
  BUILD_COMMAND
  make -C <SOURCE_DIR>/libcap SHARED=no lib=lib
  INSTALL_COMMAND
  make -C <SOURCE_DIR>/libcap SHARED=no install
  RAISE_SETFCAP=no prefix=<INSTALL_DIR> DESTDIR= lib=lib
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libcap.a
  LOG_DOWNLOAD  ON
  LOG_BUILD     ON
  LOG_OUTPUT_ON_FAILURE ON
)
ExternalProject_Get_Property(libcap_ext INSTALL_DIR)
set(CAP_LIB         "${INSTALL_DIR}/lib/libcap.a" CACHE INTERNAL "libcap static library")
set(CAP_INCLUDE_DIR "${INSTALL_DIR}/include"      CACHE INTERNAL "libcap include dir")

# ---------------------------------------------------------------------------
# GoogleTest (compiled, only when AGENTOS_BUILD_TESTS=ON)
# ---------------------------------------------------------------------------
if(AGENTOS_BUILD_TESTS)
  ExternalProject_Add(googletest_ext
    URL           https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    URL_HASH      SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
    DOWNLOAD_NAME googletest-v1.14.0.tar.gz
    PREFIX        ${DEPS_BUILD_DIR}/googletest
    DOWNLOAD_DIR  ${CMAKE_SOURCE_DIR}/deps-src/downloads
    SOURCE_DIR    ${CMAKE_SOURCE_DIR}/deps-src/googletest-src
    STAMP_DIR     ${DEPS_BUILD_DIR}/stamps/googletest
    BINARY_DIR    ${DEPS_BUILD_DIR}/googletest/build
    CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DCMAKE_CXX_STANDARD=17
    -DBUILD_SHARED_LIBS=OFF
    -DINSTALL_GTEST=ON
    -DINSTALL_GMOCK=ON
    BUILD_BYPRODUCTS
    <INSTALL_DIR>/lib/libgtest.a
    <INSTALL_DIR>/lib/libgtest_main.a
    <INSTALL_DIR>/lib/libgmock.a
    LOG_DOWNLOAD  ON
    LOG_CONFIGURE ON
    LOG_BUILD     ON
    LOG_OUTPUT_ON_FAILURE ON
  )
  ExternalProject_Get_Property(googletest_ext INSTALL_DIR)
  set(GTEST_INC      "${INSTALL_DIR}/include"             CACHE INTERNAL "googletest include path")
  set(GTEST_LIB      "${INSTALL_DIR}/lib/libgtest.a"      CACHE INTERNAL "googletest static library")
  set(GTEST_MAIN_LIB "${INSTALL_DIR}/lib/libgtest_main.a" CACHE INTERNAL "googletest main static library")
  set(GMOCK_LIB      "${INSTALL_DIR}/lib/libgmock.a"      CACHE INTERNAL "googletest mock static library")
endif()
