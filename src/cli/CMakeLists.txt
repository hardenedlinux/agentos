add_executable(agentos main.cpp)

target_link_libraries(agentos PRIVATE agentos_core)

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
