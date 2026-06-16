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
option(AGENTOS_HARDWARE_TPM "Build with hardware TPM support (standard/enterprise tier)" OFF)

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

# extra directories for ExternalProject (ADR-028)
set(DEPS_SOURCE_DIR   "${CMAKE_SOURCE_DIR}/deps-src")
set(DEPS_STAMP_DIR    "${DEPS_BUILD_DIR}/stamps")
set(DEPS_DOWNLOAD_DIR "${CMAKE_SOURCE_DIR}/deps-src/downloads")

# All third-party dependencies
add_subdirectory(deps)

# --------------------------------------------------------------------
# libtpms — always required (community tier)
# --------------------------------------------------------------------
find_package(OpenSSL REQUIRED)

ExternalProject_Add(libtpms_ext
    URL            https://github.com/stefanberger/libtpms/archive/refs/tags/v0.10.2.tar.gz
    URL_HASH       SHA256=edac03680f8a4a1c5c1d609a10e3f41e1a129e38ff5158f0c8deaedc719fb127
    SOURCE_DIR     ${DEPS_SOURCE_DIR}/libtpms
    STAMP_DIR      ${DEPS_STAMP_DIR}/libtpms
    DOWNLOAD_DIR   ${DEPS_DOWNLOAD_DIR}/libtpms
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=OFF
        -DWITH_OPENSSL=ON
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libtpms.a
)
ExternalProject_Get_Property(libtpms_ext INSTALL_DIR)
set(LIBTPMS_INSTALL_DIR ${INSTALL_DIR})

add_library(libtpms STATIC IMPORTED GLOBAL)
set_target_properties(libtpms PROPERTIES
    IMPORTED_LOCATION ${LIBTPMS_INSTALL_DIR}/lib/libtpms.a
    INTERFACE_INCLUDE_DIRECTORIES ${LIBTPMS_INSTALL_DIR}/include
)
add_dependencies(libtpms libtpms_ext)

# --------------------------------------------------------------------
# tpm2-tss — conditional (standard/enterprise tier only)
# --------------------------------------------------------------------
if(AGENTOS_HARDWARE_TPM)
    ExternalProject_Add(tpm2_tss_ext
        URL            https://github.com/tpm2-software/tpm2-tss/archive/refs/tags/4.1.3.tar.gz
        URL_HASH       SHA256=af886b4d4a0824f0d98c544dfcb905e2e2ea9b38a76c4b3e8aebc4b5e4ba3e4e
        SOURCE_DIR     ${DEPS_SOURCE_DIR}/tpm2-tss
        STAMP_DIR      ${DEPS_STAMP_DIR}/tpm2-tss
        DOWNLOAD_DIR   ${DEPS_DOWNLOAD_DIR}/tpm2-tss
        CONFIGURE_COMMAND <SOURCE_DIR>/bootstrap &&
                          <SOURCE_DIR>/configure
                              --prefix=<INSTALL_DIR>
                              --disable-shared
                              --enable-static
                              --disable-doxygen-doc
                              --disable-doxygen-man
        BUILD_COMMAND     make -j
        INSTALL_COMMAND   make install
        BUILD_BYPRODUCTS
            <INSTALL_DIR>/lib/libtss2-esys.a
            <INSTALL_DIR>/lib/libtss2-rc.a
            <INSTALL_DIR>/lib/libtss2-mu.a
            <INSTALL_DIR>/lib/libtss2-tcti-device.a
    )
    ExternalProject_Get_Property(tpm2_tss_ext INSTALL_DIR)
    set(TPM2_TSS_INSTALL_DIR ${INSTALL_DIR})

    foreach(_lib esys rc mu tcti-device)
        add_library(tss2_${_lib} STATIC IMPORTED GLOBAL)
        set_target_properties(tss2_${_lib} PROPERTIES
            IMPORTED_LOCATION ${TPM2_TSS_INSTALL_DIR}/lib/libtss2-${_lib}.a
            INTERFACE_INCLUDE_DIRECTORIES ${TPM2_TSS_INSTALL_DIR}/include
        )
        add_dependencies(tss2_${_lib} tpm2_tss_ext)
    endforeach()
endif()

# Main project — agentos target is defined inside src/CMakeLists.txt
add_subdirectory(src)

# Link deps after agentos target is defined (order matters)
target_link_libraries(agentos PRIVATE libtpms OpenSSL::Crypto)

if(AGENTOS_HARDWARE_TPM)
    foreach(_lib esys rc mu tcti-device)
        target_link_libraries(agentos PRIVATE tss2_${_lib})
    endforeach()
    target_compile_definitions(agentos PRIVATE AGENTOS_HARDWARE_TPM=1)
endif()

if(AGENTOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
