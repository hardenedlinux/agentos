add_subdirectory(core)

add_executable(agentos cli/main.cpp)
target_link_libraries(agentos PRIVATE agentos_core)
target_include_directories(agentos PRIVATE ${CMAKE_SOURCE_DIR}/src/include)
