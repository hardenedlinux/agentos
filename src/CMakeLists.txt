# Locate system libraries for sandbox (seccomp, libcap)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SECCOMP libseccomp)
    pkg_check_modules(CAP libcap)
endif()
if(NOT SECCOMP_FOUND)
    find_library(SECCOMP_LIB seccomp
        PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu
    )
    if(SECCOMP_LIB)
        set(SECCOMP_LIBRARIES "${SECCOMP_LIB}")
    endif()
else()
    set(SECCOMP_LIBRARIES "${SECCOMP_LIBRARIES}")
endif()
if(NOT CAP_FOUND)
    find_library(CAP_LIB cap
        PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu
    )
    if(CAP_LIB)
        set(CAP_LIBRARIES "${CAP_LIB}")
    endif()
else()
    set(CAP_LIBRARIES "${CAP_LIBRARIES}")
endif()
if(SECCOMP_LIBRARIES)
    message(STATUS "Found seccomp: ${SECCOMP_LIBRARIES}")
endif()
if(CAP_LIBRARIES)
    message(STATUS "Found cap: ${CAP_LIBRARIES}")
endif()
if(NOT SECCOMP_LIBRARIES)
    message(FATAL_ERROR "libseccomp not found (install libseccomp-dev)")
endif()
if(NOT CAP_LIBRARIES)
    message(FATAL_ERROR "libcap not found (install libcap-dev)")
endif()
# Make them visible in subdirectories
set(SECCOMP_LIB "${SECCOMP_LIBRARIES}" CACHE INTERNAL "seccomp library path")
set(CAP_LIB "${CAP_LIBRARIES}" CACHE INTERNAL "cap library path")
# Make them visible in subdirectories
set(SECCOMP_LIB "${SECCOMP_LIB}" CACHE INTERNAL "seccomp library path")
set(CAP_LIB "${CAP_LIB}" CACHE INTERNAL "cap library path")

add_subdirectory(core)
add_subdirectory(core/forge)

add_executable(agentos cli/main.cpp)
target_link_libraries(agentos PRIVATE agentos_core agentos_forge ${SECCOMP_LIB} ${CAP_LIB})
target_include_directories(agentos PRIVATE
  ${CMAKE_SOURCE_DIR}/src/include
  ${CMAKE_SOURCE_DIR}/src/core
)

if(AGENTOS_COVERAGE)
  target_compile_options(agentos PRIVATE --coverage -fprofile-arcs -ftest-coverage)
  target_link_options(agentos PRIVATE --coverage)
endif()
