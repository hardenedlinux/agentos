cmake_minimum_required(VERSION 3.25)
project(deps NONE)

# Build dependencies as static libraries
set(DEPS_INSTALL_DIR "${DEPS_INSTALL_DIR}" CACHE PATH "Install dir")

# fmt (needed by spdlog)
ExternalProject_Add(fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG 10.1.1
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
    -DBUILD_SHARED_LIBS=OFF
    -DFMT_TEST=OFF
    -DFMT_DOC=OFF
  BUILD_ALWAYS FALSE
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

# spdlog (depends on fmt)
ExternalProject_Add(spdlog
  DEPENDS         fmt
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.12.0
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
    -DBUILD_SHARED_LIBS=OFF
    -DSPDLOG_BUILD_SHARED=OFF
    -DSPDLOG_BUILD_EXAMPLE=OFF
    -DSPDLOG_BUILD_TESTS=OFF
    -DSPDLOG_FMT_EXTERNAL=ON
  BUILD_ALWAYS FALSE
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

# libzmq (ZeroMQ core library)
ExternalProject_Add(libzmq
  GIT_REPOSITORY https://github.com/zeromq/libzmq.git
  GIT_TAG v4.3.5
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_SHARED=OFF
    -DBUILD_STATIC=ON
    -DZMQ_BUILD_TESTS=OFF
    -DWITH_PERF_TOOL=OFF
    -DWITH_DOCS=OFF
  BUILD_ALWAYS FALSE
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

# cppzmq (C++ binding for ZeroMQ, depends on libzmq)
ExternalProject_Add(cppzmq
  DEPENDS         libzmq
  GIT_REPOSITORY https://github.com/zeromq/cppzmq.git
  GIT_TAG v4.10.0
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
    -DBUILD_SHARED_LIBS=OFF
    -DCPPZMQ_BUILD_TESTS=OFF
    -DCPPZMQ_CMAKE_SUPPRESS_INSTALL=OFF
  BUILD_ALWAYS FALSE
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)
