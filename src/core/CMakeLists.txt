set(AGENTOS_CORE_SOURCES
  dispatcher/dispatcher.cpp
  registry/registry.cpp
  verifier/verifier.cpp
  scheduler/scheduler.cpp
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
  ${LIBUV_INC}
)

target_compile_options(agentos_core PRIVATE
  -Wall -Wextra -Wpedantic -Wno-unused-parameter
  $<$<CONFIG:Release>:-O2>
  $<$<CONFIG:Debug>:-g>
  $<$<CONFIG:Debug>:-O0>
)

target_link_libraries(agentos_core PUBLIC
  ${LIBUV_LIB}
  pthread
  dl
)
