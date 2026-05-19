if(AGENTOS_BUILD_TESTS)
    add_executable(agentos_tests test_main.cpp)
    target_link_libraries(agentos_tests PRIVATE
        GTest::gtest
        GTest::gtest_main
        agentos_core
        spdlog::spdlog
        ${ZMQ_LIB}
        sqlite3
        pthread
        dl
    )
    target_include_directories(agentos_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/include)
    add_test(NAME agentos_tests COMMAND agentos_tests)
endif()
