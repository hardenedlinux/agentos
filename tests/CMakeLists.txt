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
  ${CMAKE_SOURCE_DIR}/src/include/agentos
)

# Third-party headers — marked SYSTEM so their warnings (e.g. spdlog's
# bundled fmt) are suppressed.
set(AGENTOS_TEST_SYSTEM_INCLUDES
  ${SPDLOG_INC}
  ${RAPIDJSON_INC}
  ${CPPZMQ_INC}
  ${TOMLPLUSPLUS_INC}
  ${GTEST_INC}
)

set(AGENTOS_TEST_LIBS
  agentos_core
  agentos_forge
  ${GTEST_LIB}
  ${GTEST_MAIN_LIB}
  ${GMOCK_LIB}
  ${ZMQ_LIB}
  Threads::Threads
)

# One executable per test source file.
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)

foreach(test_src ${TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)

  add_executable(${test_name} ${test_src})
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
