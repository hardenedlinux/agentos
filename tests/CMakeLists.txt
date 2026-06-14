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
# Unit tests for AgentOS.
# Built only when AGENTOS_BUILD_TESTS is ON (controlled by the parent project,
# which also calls enable_testing()).
#
# Each .cpp under tests/ becomes its own independent test executable so that
# gtest_main is linked once per binary and tests stay isolated. All targets
# are registered with CTest and reported together by `ctest`.
find_package(Threads REQUIRED)
include(GoogleTest)

# Project headers — warnings here are ours to fix, so NOT marked SYSTEM.
set(AGENTOS_TEST_INCLUDES
  ${CMAKE_SOURCE_DIR}/src/include
)

# Third-party headers — marked SYSTEM so their warnings (e.g. spdlog's
# bundled fmt) are suppressed.
set(AGENTOS_TEST_SYSTEM_INCLUDES
  ${SPDLOG_INC}
  ${RAPIDJSON_INC}
  ${CPPZMQ_INC}
  ${TOMLPLUSPLUS_INC}
  ${GTEST_INC}
  ${CLI11_INC}
  ${ZMQ_INC}
  ${OPENSSL_STATIC_INC}
)

set(AGENTOS_TEST_LIBS
  agentos_core
  agentos_forge
  ${GTEST_LIB}
  ${GTEST_MAIN_LIB}
  ${GMOCK_LIB}
  ${ZMQ_LIB}
  ${OPENSSL_STATIC_SSL}
  ${OPENSSL_STATIC_CRYPTO}
  Threads::Threads
)

# CLI layer sources needed by specific tests.
set(CLI_CLIENT_SOURCES
  ${CMAKE_SOURCE_DIR}/src/cli/cli_client.cpp
)
set(CLI_PARAMS_SOURCES
  ${CMAKE_SOURCE_DIR}/src/cli/cli_params.cpp
)

# One executable per test source file.
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)

# my_test.cpp is a temporary manual sandbox repro tool (not a gtest TEST()
# binary, has its own main()). gtest_discover_tests would run it with
# --gtest_list_tests, which instead runs its real main() and hangs. Build it
# as a plain executable, not registered with CTest.
list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/my_test\\.cpp$")

foreach(test_src ${TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)

  # Tests that use CLI layer sources need them compiled in.
  set(extra_sources "")
  if(test_name STREQUAL "test_cli_client")
    set(extra_sources ${CLI_CLIENT_SOURCES})
  elseif(test_name STREQUAL "test_cli_params")
    set(extra_sources ${CLI_PARAMS_SOURCES})
  endif()

  add_executable(${test_name} ${test_src} ${extra_sources})
  target_compile_features(${test_name} PRIVATE cxx_std_23)
  target_link_libraries(${test_name} PRIVATE ${AGENTOS_TEST_LIBS})
  target_include_directories(${test_name} PRIVATE ${AGENTOS_TEST_INCLUDES})
  target_include_directories(${test_name} SYSTEM PRIVATE
    ${AGENTOS_TEST_SYSTEM_INCLUDES})
  add_dependencies(${test_name} googletest_ext)
  if(AGENTOS_COVERAGE)
    target_compile_options(${test_name} PRIVATE --coverage -fprofile-arcs -ftest-coverage)
    target_link_options(${test_name} PRIVATE --coverage)
  endif()
  gtest_discover_tests(${test_name})
endforeach()

# my_test: temporary manual repro tool. Build only, do not register with
# CTest (see comment above). Safe to remove this block once no longer needed.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/my_test.cpp)
  add_executable(my_test ${CMAKE_SOURCE_DIR}/tests/my_test.cpp)
  target_compile_features(my_test PRIVATE cxx_std_23)
  target_link_libraries(my_test PRIVATE ${AGENTOS_TEST_LIBS})
  target_include_directories(my_test PRIVATE ${AGENTOS_TEST_INCLUDES})
  target_include_directories(my_test SYSTEM PRIVATE
    ${AGENTOS_TEST_SYSTEM_INCLUDES})
  add_dependencies(my_test googletest_ext)
endif()
