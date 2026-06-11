add_library(agentos_forge
  forge_job.cpp
  forge_database.cpp
  forge_state_machine.cpp
  forge_manager.cpp
  forge_rpc.cpp
  forge_cli.cpp
  forge_coordinator.cpp
  code_writer.cpp
  code_reviewer.cpp
)

target_link_libraries(agentos_forge PUBLIC agentos_core ${SECCOMP_LIB} ${CAP_LIB})
target_include_directories(agentos_forge PUBLIC
  ${CMAKE_SOURCE_DIR}/src/include
  ${CPPZMQ_INC}
  ${SQLITE3_SRC_DIR}
  ${SECCOMP_INCLUDE_DIR}
  ${CAP_INCLUDE_DIR}
  ${HTTPLIB_INC}
)

add_dependencies(agentos_forge libseccomp_ext libcap_ext)
