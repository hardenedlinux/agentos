#include <gtest/gtest.h>
#include "agentos/mind_layer.h"
#include "agentos/registry.h"
#include "database/database.hpp"

namespace agentos {
namespace {

class MindLayerTest : public ::testing::Test {
protected:
    class MockRegistry : public Registry {
    public:
        // No need to implement anything for these tests
    };

    class MockDatabase : public Database {
    public:
        MockDatabase() : Database(":memory:") {}
    };

    MockRegistry registry;
    MockDatabase db;
    MindLayer mindLayer{registry, db};
};

TEST_F(MindLayerTest, AnalyseTask_ReturnsNonEmptyPlan) {
    Task task;
    task.id = "test_task";
    task.goal = "search the web for quantum computing";
    task.input_json = "{}";
    auto plan = mindLayer.analyse_task(task);
    EXPECT_FALSE(plan.empty());
}

TEST_F(MindLayerTest, RequiredCapabilities_ReturnsNonEmpty) {
    std::string plan_json = R"({"steps":[{"id":"step1","command":"web.search","args":{"query":"quantum"}}]})";
    auto caps = mindLayer.required_capabilities(plan_json);
    EXPECT_FALSE(caps.empty());
}

TEST_F(MindLayerTest, RequiredCapabilities_EmptyPlan_ReturnsEmpty) {
    std::string plan_json = R"({"steps":[]})";
    auto caps = mindLayer.required_capabilities(plan_json);
    EXPECT_TRUE(caps.empty());
}

} // namespace
} // namespace agentos
// No changes needed
