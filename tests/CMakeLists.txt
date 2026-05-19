if(AGENTOS_BUILD_TESTS)
    add_executable(agentos_tests
        test_main.cpp
        test_registry.cpp
        test_verifier.cpp
        test_dispatcher.cpp
        test_rpc.cpp
        test_obs_bus.cpp
        test_database.cpp
    )

    target_include_directories(agentos_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/core
        ${RAPIDJSON_INC}
        ${SPDLOG_INC}
        ${GTEST_INC}
    )

    target_compile_options(agentos_tests PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Debug>:-O0>
    )

    target_link_libraries(agentos_tests PRIVATE
        agentos_core
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
        spdlog::spdlog
        ${ZMQ_LIB}
        sqlite3
        pthread
        dl
    )

    if(AGENTOS_STATIC)
        target_link_options(agentos_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()

    include(GoogleTest)
    gtest_discover_tests(agentos_tests)
endif()
