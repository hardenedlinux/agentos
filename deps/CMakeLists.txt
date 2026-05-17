cmake_minimum_required(VERSION 3.25)
project(agentos_deps NONE)

if(NOT DEFINED DEPS_INSTALL_DIR)
  message(FATAL_ERROR "DEPS_INSTALL_DIR must be set by the superbuild")
endif()

message(STATUS "Deps will install to: ${DEPS_INSTALL_DIR}")
include(ExternalProject)

# libuv
ExternalProject_Add(libuv
  GIT_REPOSITORY    https://github.com/libuv/libuv.git
  GIT_TAG           v1.48.0
  GIT_SHALLOW       TRUE
  GIT_PROGRESS      TRUE
  CMAKE_ARGS
  -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
  -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
  -DBUILD_SHARED_LIBS=OFF
  -DLIBUV_BUILD_SHARED=OFF
  -DLIBUV_BUILD_TESTS=OFF
  -DLIBUV_BUILD_BENCH=OFF
  INSTALL_COMMAND   ${CMAKE_COMMAND} --install <BINARY_DIR>
  LOG_DOWNLOAD      ON
  LOG_CONFIGURE     ON
  LOG_BUILD         ON
  LOG_INSTALL       ON
  LOG_OUTPUT_ON_FAILURE ON
  USES_TERMINAL_DOWNLOAD TRUE
  USES_TERMINAL_BUILD    TRUE
)

# RapidJSON (header-only)
ExternalProject_Add(rapidjson
  GIT_REPOSITORY    https://github.com/Tencent/rapidjson.git
  GIT_TAG           v1.1.0
  GIT_SHALLOW       TRUE
  GIT_PROGRESS      TRUE
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND
    ${CMAKE_COMMAND} -E copy_directory
      <SOURCE_DIR>/include ${DEPS_INSTALL_DIR}/include
  LOG_DOWNLOAD      ON
  LOG_INSTALL       ON
  LOG_OUTPUT_ON_FAILURE ON
  USES_TERMINAL_DOWNLOAD TRUE
)

# spdlog (header-only)
ExternalProject_Add(spdlog
  GIT_REPOSITORY    https://github.com/gabime/spdlog.git
  GIT_TAG           v1.13.0
  GIT_SHALLOW       TRUE
  GIT_PROGRESS      TRUE
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND
    ${CMAKE_COMMAND} -E copy_directory
      <SOURCE_DIR>/include ${DEPS_INSTALL_DIR}/include
  LOG_DOWNLOAD      ON
  LOG_INSTALL       ON
  LOG_OUTPUT_ON_FAILURE ON
  USES_TERMINAL_DOWNLOAD TRUE
)

# GoogleTest
ExternalProject_Add(googletest
  GIT_REPOSITORY    https://github.com/google/googletest.git
  GIT_TAG           v1.14.0
  GIT_SHALLOW       TRUE
  GIT_PROGRESS      TRUE
  CMAKE_ARGS
  -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
  -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
  -DBUILD_SHARED_LIBS=OFF
  -DBUILD_GMOCK=ON
  -Dgtest_force_shared_crt=OFF
  -DINSTALL_GTEST=ON
  INSTALL_COMMAND   ${CMAKE_COMMAND} --install <BINARY_DIR>
  LOG_DOWNLOAD      ON
  LOG_CONFIGURE     ON
  LOG_BUILD         ON
  LOG_INSTALL       ON
  LOG_OUTPUT_ON_FAILURE ON
  USES_TERMINAL_DOWNLOAD TRUE
  USES_TERMINAL_BUILD    TRUE
)

#  stduuid
ExternalProject_Add(stduuid
  GIT_REPOSITORY    https://github.com/mariusbancila/stduuid.git
  GIT_TAG           v1.2.3
  GIT_SHALLOW       TRUE
  GIT_PROGRESS      TRUE
  CMAKE_ARGS
  -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
  -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
  -DUUID_BUILD_TESTS=OFF
  INSTALL_COMMAND   ${CMAKE_COMMAND} --install <BINARY_DIR>
  LOG_DOWNLOAD      ON
  LOG_CONFIGURE     ON
  LOG_BUILD         ON
  LOG_INSTALL       ON
  LOG_OUTPUT_ON_FAILURE ON
  USES_TERMINAL_DOWNLOAD TRUE
  USES_TERMINAL_BUILD    TRUE
)
