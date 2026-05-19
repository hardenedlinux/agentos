set(AGENTOS_CORE_SOURCES
  dispatcher/dispatcher.cpp
  dispatcher/rpc.cpp
  registry/registry.cpp
  verifier/verifier.cpp
  scheduler/scheduler.cpp
  master/master.cpp
  orchestrator/orchestrator.cpp
  obs_bus/obs_bus.cpp
)

add_library(agentos_core STATIC ${AGENTOS_CORE_SOURCES})

target_include_directories(agentos_core PUBLIC
  PUBLIC
  ${CMAKE_SOURCE_DIR}/include

  SYSTEM PUBLIC
  ${RAPIDJSON_INC}
  ${SPDLOG_INC}
)

target_compile_options(agentos_core PRIVATE
  -Wall -Wextra -Wpedantic -Wno-unused-parameter
  $<$<CONFIG:Release>:-O2>
  $<$<CONFIG:Debug>:-g>
  $<$<CONFIG:Debug>:-O0>
)

# Find static SQLite3 library
find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
find_library(SQLITE3_LIBRARY NAMES sqlite3 libsqlite3
  PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
  NO_DEFAULT_PATH
)
if(NOT SQLITE3_LIBRARY)
  message(FATAL_ERROR "Could not find static libsqlite3.a")
endif()

target_include_directories(agentos_core SYSTEM PRIVATE ${SQLITE3_INCLUDE_DIR})

target_link_libraries(agentos_core PUBLIC
  pthread
  dl
  ${SQLITE3_LIBRARY}
)
