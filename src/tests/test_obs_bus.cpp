#include <gtest/gtest.h>
#include "agentos/obs_bus.h"

using namespace agentos;

TEST(ObsBusTest, EmitLogRecord) {
    ObsBus bus;
    LogRecord rec;
    rec.level = LogLevel::INFO;
    rec.subsystem = "test";
    rec.message = "test message";
    EXPECT_NO_THROW(bus.emit(rec));
}

TEST(ObsBusTest, TaskSubmitted) {
    ObsBus bus;
    EXPECT_NO_THROW(bus.task_submitted("task1", "goal1"));
}

TEST(ObsBusTest, TaskCompleted) {
    ObsBus bus;
    TaskResult result;
    result.task_id = "task1";
    result.success = true;
    EXPECT_NO_THROW(bus.task_completed(result));
    result.success = false;
    result.error = "error";
    EXPECT_NO_THROW(bus.task_completed(result));
}

TEST(ObsBusTest, StepStarted) {
    ObsBus bus;
    EXPECT_NO_THROW(bus.step_started("task1", "step1", "cmd1"));
}

TEST(ObsBusTest, StepCompleted) {
    ObsBus bus;
    EXPECT_NO_THROW(bus.step_completed("task1", "step1", true));
    EXPECT_NO_THROW(bus.step_completed("task1", "step1", false));
}

TEST(ObsBusTest, ClientConnected) {
    ObsBus bus;
    EXPECT_NO_THROW(bus.client_connected("client1", ClientType::Adviser));
    EXPECT_NO_THROW(bus.client_connected("client2", ClientType::Worker));
}

TEST(ObsBusTest, ClientDisconnected) {
    ObsBus bus;
    EXPECT_NO_THROW(bus.client_disconnected("client1"));
}
