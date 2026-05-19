#include <gtest/gtest.h>
#include "agentos/verifier.h"
#include "agentos/registry.h"

using namespace agentos;

// Helper: build a registry with a web.search command
static Registry make_registry() {
    Registry reg;
    RegisteredExecutor ex;
    ex.id   = "ex-1";
    ex.name = "web-search";

    CommandSchema cs;
    cs.name        = "web.search";
    cs.description = "Searches the web";
    ArgSchema q; q.type = "string"; q.required = true;  q.description = "query";
    ArgSchema n; n.type = "integer"; n.required = false; n.description = "max results";
    cs.input["query"]       = q;
    cs.input["max_results"] = n;
    ex.commands.push_back(cs);

    reg.register_worker(ex);
    return reg;
}

TEST(Verifier, ValidPlanPasses) {
    auto reg = make_registry();
    Verifier v(reg);

    Plan plan;
    plan.task_id = "t1";
    plan.steps.push_back({ "s1", "web.search", {{"query", "test"}}, {} });

    auto r = v.verify(plan);
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.errors.empty());
}

TEST(Verifier, UnknownCommandFails) {
    auto reg = make_registry();
    Verifier v(reg);

    Plan plan;
    plan.task_id = "t1";
    plan.steps.push_back({ "s1", "nonexistent.cmd", {}, {} });

    auto r = v.verify(plan);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.errors.empty());
}

TEST(Verifier, MissingRequiredArgFails) {
    auto reg = make_registry();
    Verifier v(reg);

    Plan plan;
    plan.task_id = "t1";
    // 'query' is required but not provided
    plan.steps.push_back({ "s1", "web.search", {}, {} });

    auto r = v.verify(plan);
    EXPECT_FALSE(r.ok);
}

TEST(Verifier, InvalidDependencyFails) {
    auto reg = make_registry();
    Verifier v(reg);

    Plan plan;
    plan.task_id = "t1";
    plan.steps.push_back({ "s1", "web.search", {{"query","x"}}, {"nonexistent_step"} });

    auto r = v.verify(plan);
    EXPECT_FALSE(r.ok);
}

TEST(Verifier, ValidVariableRefPasses) {
    auto reg = make_registry();
    Verifier v(reg);

    Plan plan;
    plan.task_id = "t1";
    plan.steps.push_back({ "s1", "web.search", {{"query", "initial"}}, {} });
    plan.steps.push_back({ "s2", "web.search", {{"query", "{{s1.result}}"}}, {"s1"} });

    auto r = v.verify(plan);
    EXPECT_TRUE(r.ok);
}

TEST(Verifier, InvalidVariableRefFails) {
    auto reg = make_registry();
    Verifier v(reg);

    Plan plan;
    plan.task_id = "t1";
    // References a step that doesn't exist
    plan.steps.push_back({ "s1", "web.search", {{"query", "{{ghost_step.result}}"}}, {} });

    auto r = v.verify(plan);
    EXPECT_FALSE(r.ok);
}
