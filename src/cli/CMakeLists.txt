target_include_directories(agentos PRIVATE
  ${CLI11_INC}
  ${CPPZMQ_INC}
  ${ZMQ_INC}
  ${RAPIDJSON_INC}
  ${OPENSSL_STATIC_INC}
)

target_link_libraries(agentos PRIVATE
  agentos_core
  ${ZMQ_LIB}
  ${OPENSSL_STATIC_SSL}
  ${OPENSSL_STATIC_CRYPTO}
)

target_sources(agentos PRIVATE user_commands.cpp)

add_dependencies(agentos
  cli11_ext
  cppzmq_ext
  libzmq_ext
  openssl_ext
)

if(AGENTOS_COVERAGE)
  target_compile_options(agentos PRIVATE --coverage -fprofile-arcs -ftest-coverage)
  target_link_options(agentos PRIVATE --coverage)
endif()

if(AGENTOS_STATIC AND NOT AGENTOS_MUSL)
  target_link_options(agentos PRIVATE
    -static-libgcc
    -static-libstdc++
  )
endif()

if(AGENTOS_MUSL)
  target_link_options(agentos PRIVATE -static)
endif()

if(AGENTOS_STRIP AND CMAKE_BUILD_TYPE STREQUAL "Release")
  add_custom_command(TARGET agentos POST_BUILD
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:agentos>
    COMMENT "Stripping debug symbols")
endif()

add_custom_command(TARGET agentos POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E echo "--- Build complete ---"
  COMMAND ls -lh $<TARGET_FILE:agentos>
)
