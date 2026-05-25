set(AGENTOS_CORE_SOURCES
  dispatcher/dispatcher.cpp
  dispatcher/rpc.cpp
  registry/registry.cpp
  verifier/verifier.cpp
  scheduler/scheduler.cpp
  master/master.cpp
  orchestrator/orchestrator.cpp
  obs_bus/obs_bus.cpp
  database/database.cpp
  capability/capability.cpp
  enforce_layer.cpp
  mind_layer.cpp
  sandbox/sandbox.cpp
  llm/llm_client.cpp
  config/config.cpp
  home_init.cpp
)

add_library(sqlite3 STATIC ${SQLITE3_SRC_DIR}/sqlite3.c)

target_include_directories(sqlite3 SYSTEM PRIVATE ${SQLITE3_SRC_DIR})

target_compile_definitions(sqlite3 PRIVATE
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_COMPLETE
)

set_source_files_properties(${SQLITE3_SRC_DIR}/sqlite3.c PROPERTIES COMPILE_FLAGS "-Wno-implicit-fallthrough")

add_library(agentos_core STATIC ${AGENTOS_CORE_SOURCES})

target_include_directories(agentos_core PUBLIC
  PUBLIC
  ${CMAKE_SOURCE_DIR}/src/include

  SYSTEM PUBLIC
  ${RAPIDJSON_INC}
  ${SPDLOG_INC}
  ${HTTPLIB_INC}
  ${CMAKE_SOURCE_DIR}/deps-src/httplib-src
  ${CPPZMQ_INC}
  ${TOMLPLUSPLUS_INC}
)


target_include_directories(agentos_core SYSTEM PRIVATE ${SQLITE3_SRC_DIR})

target_compile_definitions(agentos_core PRIVATE
  SQLITE_ENABLE_JSON1
  SQLITE_ENABLE_FTS5
  SQLITE_ENABLE_RTREE
  CPPHTTPLIB_OPENSSL_SUPPORT
)

target_compile_options(agentos_core PRIVATE
  -Wall -Wextra -Wpedantic -Wno-unused-parameter
  -Wno-deprecated-declarations
  -Wno-class-memaccess
  -Wno-array-bounds
  $<$<CONFIG:Release>:-O2>
  $<$<CONFIG:Debug>:-g>
  $<$<CONFIG:Debug>:-O0>
)

target_link_libraries(agentos_core PUBLIC
  pthread
  dl
  ${SPDLOG_LIB}
  ${ZMQ_LIB}
  ${SECCOMP_LIB}
  ${CAP_LIB}
  OpenSSL::SSL
  OpenSSL::Crypto
)

target_include_directories(agentos_core PUBLIC
  ${SECCOMP_INCLUDE_DIR}
  ${CAP_INCLUDE_DIR}
)

add_dependencies(agentos_core libseccomp_ext libcap_ext spdlog_ext httplib_ext rapidjson_ext libzmq_ext cppzmq_ext)

target_link_libraries(agentos_core PRIVATE sqlite3)

if(AGENTOS_COVERAGE)
  target_compile_options(agentos_core PRIVATE --coverage -fprofile-arcs -ftest-coverage)
  target_link_options(agentos_core PRIVATE --coverage)
endif()
