add_library(agentos_forge
    forge_job.cpp
    forge_database.cpp
    forge_state_machine.cpp
    forge_manager.cpp
    forge_rpc.cpp
    forge_cli.cpp
)

target_link_libraries(agentos_forge PUBLIC agentos_core)
target_include_directories(agentos_forge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/core/database
    ${CMAKE_SOURCE_DIR}/src/core/registry
    ${CMAKE_SOURCE_DIR}/src/core/dispatcher
    ${CMAKE_SOURCE_DIR}/src/core/orchestrator
    ${CMAKE_SOURCE_DIR}/src/core/obs_bus
    ${CPPZMQ_INC}
    ${SQLITE3_SRC_DIR}
)
