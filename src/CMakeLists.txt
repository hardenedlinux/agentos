# SECCOMP_LIB and CAP_LIB are set by the top-level CMakeLists.txt via ExternalProject
# No need to locate them here.

add_subdirectory(core)
add_subdirectory(core/forge)

file(GLOB CLI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cli/*.cpp)
add_executable(agentos ${CLI_SOURCES})

target_link_libraries(agentos PRIVATE agentos_core agentos_forge ${SECCOMP_LIB} ${CAP_LIB})

target_include_directories(agentos PRIVATE
  ${DEPS_INSTALL_DIR}/include
)

target_include_directories(agentos SYSTEM PRIVATE
  ${SPDLOG_INC}
  ${RAPIDJSON_INC}
  ${HTTPLIB_INC}
  ${CPPZMQ_INC}
  ${TOMLPLUSPLUS_INC}
  ${CLI11_INC}
)

add_dependencies(agentos httplib_ext rapidjson_ext spdlog_ext cppzmq_ext)

if(AGENTOS_COVERAGE)
  target_compile_options(agentos PRIVATE --coverage -fprofile-arcs -ftest-coverage)
  target_link_options(agentos PRIVATE --coverage)
endif()
