# SECCOMP_LIB and CAP_LIB are set by the top-level CMakeLists.txt via ExternalProject
# No need to locate them here.

add_subdirectory(core)
add_subdirectory(core/forge)

add_executable(agentos cli/main.cpp)
target_link_libraries(agentos PRIVATE agentos_core agentos_forge ${SECCOMP_LIB} ${CAP_LIB})
target_include_directories(agentos PRIVATE
  ${CMAKE_SOURCE_DIR}/src/include
  ${CMAKE_SOURCE_DIR}/src/core
  ${HTTPLIB_INC}
  ${CMAKE_SOURCE_DIR}/deps-src/httplib-src
  ${RAPIDJSON_INC}
  ${SPDLOG_INC}
  ${CPPZMQ_INC}
)
add_dependencies(agentos httplib_ext rapidjson_ext spdlog_ext cppzmq_ext)

if(AGENTOS_COVERAGE)
  target_compile_options(agentos PRIVATE --coverage -fprofile-arcs -ftest-coverage)
  target_link_options(agentos PRIVATE --coverage)
endif()
