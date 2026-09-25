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
# Header-only deps (rapidjson, cppzmq, httplib, tomlplusplus, CLI11) have no
# build step and are not affected by the standard either way.
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
#
# All deps are compiled statically and linked into a single binary.
# No system libraries are used except the Linux kernel ABI (libc, libm,
# libpthread, libdl) which are part of the OS ABI and cannot be avoided.

include(ExternalProject)
include(FetchContent)

# ---------------------------------------------------------------------------
# spdlog (compiled, static)
# ---------------------------------------------------------------------------
ExternalProject_Add(spdlog_ext
  URL           https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.tar.gz
  URL_HASH      SHA256=15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67
  DOWNLOAD_NAME spdlog-v1.15.3.tar.gz
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
  URL           https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.39.0.tar.gz
  URL_HASH      SHA256=44ef684e2c97d034ac1a3dde58407b7017471abd9611d1a9b562d22044bee322
  DOWNLOAD_NAME httplib-v0.39.0.tar.gz
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
# CLI11 (header-only, extracted at configure time)
# ---------------------------------------------------------------------------
set(CLI11_SRC_DIR "${CMAKE_SOURCE_DIR}/deps-src/cli11-src"
  CACHE INTERNAL "CLI11 source directory")
set(CLI11_DOWNLOAD_PATH
  "${CMAKE_SOURCE_DIR}/deps-src/downloads/cli11-v2.4.2.tar.gz")

if(NOT EXISTS "${CLI11_SRC_DIR}/include/CLI/CLI.hpp")
  if(NOT EXISTS "${CLI11_DOWNLOAD_PATH}")
    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/deps-src/downloads")
    message(STATUS "Downloading CLI11...")
    file(DOWNLOAD
      https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.4.2.tar.gz
      "${CLI11_DOWNLOAD_PATH}"
      EXPECTED_HASH SHA256=f2d893a65c3b1324c50d4e682c0cdc021dd0477ae2c048544f39eed6654b699a
      STATUS download_status
      LOG    download_log
    )
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
      message(FATAL_ERROR "Failed to download CLI11: ${download_log}")
    endif()
  endif()
  set(_cli11_extracted "${CMAKE_SOURCE_DIR}/deps-src/CLI11-2.4.2")
  if(EXISTS "${_cli11_extracted}")
    file(REMOVE_RECURSE "${_cli11_extracted}")
  endif()
  message(STATUS "Extracting CLI11...")
  file(ARCHIVE_EXTRACT
    INPUT       "${CLI11_DOWNLOAD_PATH}"
    DESTINATION "${CMAKE_SOURCE_DIR}/deps-src"
  )
  file(RENAME "${_cli11_extracted}" "${CLI11_SRC_DIR}")
endif()

set(CLI11_INC "${CLI11_SRC_DIR}/include" CACHE INTERNAL "CLI11 include path")

# ---------------------------------------------------------------------------
# toml++ (header-only, single file download)
# ---------------------------------------------------------------------------
set(TOMLPLUSPLUS_DOWNLOAD_DIR "${CMAKE_SOURCE_DIR}/deps-src/downloads")
set(TOMLPLUSPLUS_FILE "${TOMLPLUSPLUS_DOWNLOAD_DIR}/toml.hpp")
if(NOT EXISTS "${TOMLPLUSPLUS_FILE}")
  file(MAKE_DIRECTORY "${TOMLPLUSPLUS_DOWNLOAD_DIR}")
  file(DOWNLOAD
    https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp
    "${TOMLPLUSPLUS_FILE}"
    STATUS download_status
    LOG    download_log
  )
  list(GET download_status 0 download_code)
  if(NOT download_code EQUAL 0)
    message(FATAL_ERROR "Failed to download toml.hpp: ${download_log}")
  endif()
endif()
set(TOMLPLUSPLUS_DIR "${CMAKE_SOURCE_DIR}/deps-src/tomlplusplus")
file(MAKE_DIRECTORY "${TOMLPLUSPLUS_DIR}")
file(COPY "${TOMLPLUSPLUS_FILE}" DESTINATION "${TOMLPLUSPLUS_DIR}")
set(TOMLPLUSPLUS_INC "${TOMLPLUSPLUS_DIR}" CACHE INTERNAL "tomlplusplus include path")

# ---------------------------------------------------------------------------
# SQLite3 amalgamation (compiled directly into agentos_core)
# ---------------------------------------------------------------------------
set(SQLITE3_SRC_DIR "${CMAKE_SOURCE_DIR}/deps-src/sqlite3_amalgamation-src"
  CACHE INTERNAL "sqlite3 source directory")
set(SQLITE3_DOWNLOAD_PATH
  "${CMAKE_SOURCE_DIR}/deps-src/downloads/sqlite3-amalgamation-3490100.zip")

if(NOT EXISTS "${SQLITE3_SRC_DIR}/sqlite3.c")
  if(NOT EXISTS "${SQLITE3_DOWNLOAD_PATH}")
    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/deps-src/downloads")
    message(STATUS "Downloading sqlite3 amalgamation...")
    file(DOWNLOAD
      https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip
      "${SQLITE3_DOWNLOAD_PATH}"
      EXPECTED_HASH SHA256=6cebd1d8403fc58c30e93939b246f3e6e58d0765a5cd50546f16c00fd805d2c3
      STATUS download_status
      LOG    download_log
    )
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
      message(FATAL_ERROR "Failed to download sqlite3: ${download_log}")
    endif()
  endif()
  message(STATUS "Extracting sqlite3 amalgamation...")
  file(ARCHIVE_EXTRACT
    INPUT "${SQLITE3_DOWNLOAD_PATH}"
    DESTINATION "${CMAKE_SOURCE_DIR}/deps-src"
  )
  file(RENAME
    "${CMAKE_SOURCE_DIR}/deps-src/sqlite-amalgamation-3490100"
    "${SQLITE3_SRC_DIR}"
  )
endif()

# ---------------------------------------------------------------------------
# libseccomp (C only, autoconf/make)
# ---------------------------------------------------------------------------
ExternalProject_Add(libseccomp_ext
  URL           https://github.com/seccomp/libseccomp/archive/refs/tags/v2.6.0.tar.gz
  URL_HASH      SHA256=0889a8da98e37f86019c90789fd4ff7eda6e1ceb9ef07d4c51c67aeb50a77860
  DOWNLOAD_NAME libseccomp-v2.6.0.tar.gz
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
# ---------------------------------------------------------------------------
ExternalProject_Add(libcap_ext
  URL           https://git.kernel.org/pub/scm/libs/libcap/libcap.git/snapshot/libcap-2.74.tar.gz
  URL_HASH      SHA256=6b7a77c463f8be6876a99ecaeb7fbc20303270aed6ca4109ad65805513df5fd9
  DOWNLOAD_NAME libcap-v2.74.tar.gz
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
# OpenSSL (compiled, static) — no system OpenSSL is used anywhere
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(OPENSSL_TARGET "linux-aarch64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  set(OPENSSL_TARGET "linux-x86_64")
else()
  message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

ExternalProject_Add(openssl_ext
  URL           https://github.com/openssl/openssl/releases/download/openssl-3.3.2/openssl-3.3.2.tar.gz
  URL_HASH      SHA256=2e8a40b01979afe8be0bbfb3de5dc1c6709fedb46d6c89c10da114ab5fc3d281
  DOWNLOAD_NAME openssl-3.3.2.tar.gz
  PREFIX        ${DEPS_BUILD_DIR}/openssl
  DOWNLOAD_DIR  ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR    ${CMAKE_SOURCE_DIR}/deps-src/openssl-src
  STAMP_DIR     ${DEPS_BUILD_DIR}/stamps/openssl
  BINARY_DIR    ${DEPS_BUILD_DIR}/openssl/build
  CONFIGURE_COMMAND
    <SOURCE_DIR>/Configure
    --prefix=<INSTALL_DIR>
    --openssldir=<INSTALL_DIR>/ssl
    --libdir=<INSTALL_DIR>/lib
    no-shared
    no-tests
    ${OPENSSL_TARGET}
  BUILD_COMMAND   make -j4
  INSTALL_COMMAND make install_sw
  BUILD_BYPRODUCTS
    <INSTALL_DIR>/lib/libssl.a
    <INSTALL_DIR>/lib/libcrypto.a
  LOG_DOWNLOAD  ON
  LOG_CONFIGURE ON
  LOG_BUILD     ON
  LOG_OUTPUT_ON_FAILURE ON
)
ExternalProject_Get_Property(openssl_ext INSTALL_DIR)
set(OPENSSL_STATIC_SSL    "${INSTALL_DIR}/lib/libssl.a"    CACHE INTERNAL "openssl ssl")
set(OPENSSL_STATIC_CRYPTO "${INSTALL_DIR}/lib/libcrypto.a" CACHE INTERNAL "openssl crypto")
set(OPENSSL_STATIC_INC    "${INSTALL_DIR}/include"         CACHE INTERNAL "openssl include")

# ---------------------------------------------------------------------------
# libtpms — always required (community tier, software TPM emulator)
# Uses autotools (autogen.sh + configure + make), NOT CMake.
# Depends on openssl_ext — must use the static OpenSSL we built, not system.
# ---------------------------------------------------------------------------
ExternalProject_Add(libtpms_ext
  URL           https://github.com/stefanberger/libtpms/archive/refs/tags/v0.10.2.tar.gz
  URL_HASH      SHA256=edac03680f8a4a1c5c1d609a10e3f41e1a129e38ff5158f0c8deaedc719fb127
  DOWNLOAD_NAME libtpms-v0.10.2.tar.gz
  PREFIX        ${DEPS_BUILD_DIR}/libtpms
  DOWNLOAD_DIR  ${CMAKE_SOURCE_DIR}/deps-src/downloads
  SOURCE_DIR    ${CMAKE_SOURCE_DIR}/deps-src/libtpms-src
  STAMP_DIR     ${DEPS_BUILD_DIR}/stamps/libtpms
  BUILD_IN_SOURCE TRUE
  DEPENDS       openssl_ext
  CONFIGURE_COMMAND
    ./autogen.sh
      --prefix=<INSTALL_DIR>
      --with-openssl
      --disable-shared
      --enable-static
      PKG_CONFIG_PATH=${OPENSSL_STATIC_INC}/../lib/pkgconfig
  BUILD_COMMAND   make -j4
  INSTALL_COMMAND make install
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libtpms.a
  LOG_DOWNLOAD  ON
  LOG_CONFIGURE ON
  LOG_BUILD     ON
  LOG_OUTPUT_ON_FAILURE ON
)
ExternalProject_Get_Property(libtpms_ext INSTALL_DIR)
set(LIBTPMS_LIB "${INSTALL_DIR}/lib/libtpms.a" CACHE INTERNAL "libtpms static library")
set(LIBTPMS_INC "${INSTALL_DIR}/include"        CACHE INTERNAL "libtpms include path")

# ---------------------------------------------------------------------------
# tpm2-tss — conditional (standard/enterprise tier only)
# Also depends on openssl_ext for static linking.
# ---------------------------------------------------------------------------
if(AGENTOS_HARDWARE_TPM)
  ExternalProject_Add(tpm2_tss_ext
    URL           https://github.com/tpm2-software/tpm2-tss/archive/refs/tags/4.1.3.tar.gz
    URL_HASH      SHA256=af886b4d4a0824f0d98c544dfcb905e2e2ea9b38a76c4b3e8aebc4b5e4ba3e4e
    DOWNLOAD_NAME tpm2-tss-4.1.3.tar.gz
    PREFIX        ${DEPS_BUILD_DIR}/tpm2-tss
    DOWNLOAD_DIR  ${CMAKE_SOURCE_DIR}/deps-src/downloads
    SOURCE_DIR    ${CMAKE_SOURCE_DIR}/deps-src/tpm2-tss-src
    STAMP_DIR     ${DEPS_BUILD_DIR}/stamps/tpm2-tss
    DEPENDS       openssl_ext
    CONFIGURE_COMMAND
      <SOURCE_DIR>/bootstrap &&
      <SOURCE_DIR>/configure
        --prefix=<INSTALL_DIR>
        --disable-shared
        --enable-static
        --disable-doxygen-doc
        --disable-doxygen-man
        PKG_CONFIG_PATH=${OPENSSL_STATIC_INC}/../lib/pkgconfig
    BUILD_COMMAND   make -j
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS
      <INSTALL_DIR>/lib/libtss2-esys.a
      <INSTALL_DIR>/lib/libtss2-rc.a
      <INSTALL_DIR>/lib/libtss2-mu.a
      <INSTALL_DIR>/lib/libtss2-tcti-device.a
    LOG_DOWNLOAD  ON
    LOG_CONFIGURE ON
    LOG_BUILD     ON
    LOG_OUTPUT_ON_FAILURE ON
  )
  ExternalProject_Get_Property(tpm2_tss_ext INSTALL_DIR)
  set(TPM2_TSS_INSTALL_DIR ${INSTALL_DIR})

  foreach(_lib esys rc mu tcti-device)
    add_library(tss2_${_lib} STATIC IMPORTED GLOBAL)
    set_target_properties(tss2_${_lib} PROPERTIES
      IMPORTED_LOCATION             ${TPM2_TSS_INSTALL_DIR}/lib/libtss2-${_lib}.a
      INTERFACE_INCLUDE_DIRECTORIES ${TPM2_TSS_INSTALL_DIR}/include
    )
    add_dependencies(tss2_${_lib} tpm2_tss_ext)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# GoogleTest (compiled, only when AGENTOS_BUILD_TESTS=ON)
# ---------------------------------------------------------------------------
if(AGENTOS_BUILD_TESTS)
  ExternalProject_Add(googletest_ext
    URL           https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH      SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_NAME googletest-v1.17.0.tar.gz
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
