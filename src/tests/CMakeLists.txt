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

# Automatically discover all test source files in this directory.
# Any .cpp file placed here will be compiled into the test executable.
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(agentos_tests ${TEST_SOURCES})

target_include_directories(agentos_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/core
  ${RAPIDJSON_INC}
  ${SPDLOG_INC}
  ${GTEST_INC}
)

target_compile_options(agentos_tests PRIVATE
  -Wall -Wextra -Wno-unused-parameter
  $<$<CONFIG:Release>:-O2>
  $<$<CONFIG:Debug>:-g>
  $<$<CONFIG:Debug>:-O0>
)

target_link_libraries(agentos_tests PRIVATE
  agentos_core
  GTest::gtest
  GTest::gtest_main
  GTest::gmock
)

if(AGENTOS_STATIC)
  target_link_options(agentos_tests PRIVATE -static-libgcc -static-libstdc++)
endif()

if(AGENTOS_COVERAGE)
  target_compile_options(agentos_tests PRIVATE --coverage -fprofile-arcs -ftest-coverage)
  target_link_options(agentos_tests PRIVATE --coverage)
endif()

include(GoogleTest)
gtest_discover_tests(agentos_tests)
