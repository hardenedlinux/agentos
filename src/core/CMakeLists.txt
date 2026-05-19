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
  ${CMAKE_SOURCE_DIR}/include

  SYSTEM PUBLIC
  ${RAPIDJSON_INC}
  ${SPDLOG_INC}
)

target_include_directories(agentos_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(agentos_core SYSTEM PRIVATE ${SQLITE3_SRC_DIR})

target_compile_definitions(agentos_core PRIVATE
  SQLITE_ENABLE_JSON1
  SQLITE_ENABLE_FTS5
  SQLITE_ENABLE_RTREE
)

target_compile_options(agentos_core PRIVATE
  -Wall -Wextra -Wpedantic -Wno-unused-parameter
  $<$<CONFIG:Release>:-O2>
  $<$<CONFIG:Debug>:-g>
  $<$<CONFIG:Debug>:-O0>
)

target_link_libraries(agentos_core PUBLIC
  pthread
  dl
  spdlog::spdlog
  ${ZMQ_LIB}
)

target_link_libraries(agentos_core PRIVATE sqlite3)
