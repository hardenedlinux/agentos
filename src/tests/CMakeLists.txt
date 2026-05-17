add_executable(agentos_tests
  test_registry.cpp
  test_verifier.cpp
)

target_include_directories(agentos_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
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
  ${GTEST_MAIN}
  ${GTEST_LIB}
  ${GMOCK_LIB}
  ${LIBUV_LIB}
  pthread
  dl
)

if(AGENTOS_STATIC)
  target_link_options(agentos_tests PRIVATE -static-libgcc -static-libstdc++)
endif()

include(GoogleTest)
gtest_discover_tests(agentos_tests)
