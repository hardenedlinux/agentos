#include <gtest/gtest.h>
#include "agentos/dispatcher.h"
#include <filesystem>
#include <thread>
#include <chrono>

using namespace agentos;

TEST(DispatcherTest, BindAndStop) {
    std::string tmp_dir = "/tmp/agentos_test_" + std::to_string(getpid());
    std::filesystem::create_directories(tmp_dir);
    Dispatcher dispatcher(tmp_dir);
    EXPECT_TRUE(dispatcher.bind());
    dispatcher.stop();
    std::filesystem::remove_all(tmp_dir);
}

TEST(DispatcherTest, CreateAndCloseTaskPush) {
    std::string tmp_dir = "/tmp/agentos_test_" + std::to_string(getpid());
    std::filesystem::create_directories(tmp_dir);
    Dispatcher dispatcher(tmp_dir);
    ASSERT_TRUE(dispatcher.bind());
    std::string task_id = "test_task";
    std::string path = dispatcher.create_task_push(task_id);
    EXPECT_FALSE(path.empty());
    dispatcher.close_task_push(task_id);
    dispatcher.stop();
    std::filesystem::remove_all(tmp_dir);
}


TEST(DispatcherTest, BroadcastEvent) {
    std::string tmp_dir = "/tmp/agentos_test_" + std::to_string(getpid());
    std::filesystem::create_directories(tmp_dir);
    Dispatcher dispatcher(tmp_dir);
    ASSERT_TRUE(dispatcher.bind());
    std::string event_json = R"({"event":"test"})";
    EXPECT_NO_THROW(dispatcher.broadcast_event(event_json));
    dispatcher.stop();
    std::filesystem::remove_all(tmp_dir);
}
