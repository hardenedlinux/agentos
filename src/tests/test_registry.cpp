#include <gtest/gtest.h>
#include "agentos/registry.h"

using namespace agentos;

static RegisteredExecutor make_executor(const std::string& id, const std::string& name,
                                         std::vector<std::string> cmds) {
    RegisteredExecutor ex;
    ex.id   = id;
    ex.name = name;
    for (auto& c : cmds) {
        CommandSchema cs;
        cs.name        = c;
        cs.description = "test command " + c;
        ex.commands.push_back(cs);
    }
    return ex;
}

static RegisteredAdviser make_adviser(const std::string& id, const std::string& name,
                                   std::vector<std::string> domains) {
    RegisteredAdviser a;
    a.id      = id;
    a.name    = name;
    a.domains = domains;
    return a;
}

TEST(Registry, RegisterAndFindExecutor) {
    Registry reg;
    reg.register_executor(make_executor("ex-1", "web-search", {"web.search", "web.fetch"}));

    auto ex = reg.find_executor_for_command("web.search");
    ASSERT_TRUE(ex.has_value());
    EXPECT_EQ(ex->id, "ex-1");
}

TEST(Registry, UnknownCommandReturnsNullopt) {
    Registry reg;
    EXPECT_FALSE(reg.find_executor_for_command("nonexistent.cmd").has_value());
}

TEST(Registry, RegisterAndFindAgent) {
    Registry reg;
    reg.register_adviser(make_adviser("ag-1", "research-agent", {"research", "general"}));

    auto ag = reg.find_adviser("research");
    ASSERT_TRUE(ag.has_value());
    EXPECT_EQ(ag->id, "ag-1");
}

TEST(Registry, UnknownDomainReturnsNullopt) {
    Registry reg;
    EXPECT_FALSE(reg.find_adviser("coding").has_value());
}

TEST(Registry, RemoveExecutorUnregistersCommands) {
    Registry reg;
    reg.register_executor(make_executor("ex-1", "web-search", {"web.search"}));
    ASSERT_TRUE(reg.find_executor_for_command("web.search").has_value());

    reg.remove("ex-1");
    EXPECT_FALSE(reg.find_executor_for_command("web.search").has_value());
}

TEST(Registry, AllCommandSchemas) {
    Registry reg;
    reg.register_executor(make_executor("ex-1", "web",  {"web.search", "web.fetch"}));
    reg.register_executor(make_executor("ex-2", "file", {"file.write", "file.read"}));

    auto schemas = reg.all_command_schemas();
    EXPECT_EQ(schemas.size(), 4u);
}

TEST(Registry, Counts) {
    Registry reg;
    reg.register_adviser(make_adviser("ag-1", "adviser", {"general"}));
    reg.register_executor(make_executor("ex-1", "exec", {"cmd.run"}));
    EXPECT_EQ(reg.adviser_count(),    1u);
    EXPECT_EQ(reg.executor_count(), 1u);
}
