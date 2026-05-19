/**
 * tests/registry_test.cpp
 *
 * Unit tests for the static SQLite-based Registry (ADR-007).
 */

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <cstdio>
#include <string>
#include <vector>

#include "agentos/registry.h"
#include "agentos/types.h"

using namespace agentos;

// ---------------------------------------------------------------------------
// Helper: create a temporary SQLite database with the ADR-007 schema and
// populate it with the given agents.
// ---------------------------------------------------------------------------
static void create_test_db(const std::string &db_path,
                           const std::vector<std::string> &insert_statements)
{
  sqlite3 *db = nullptr;
  int rc = sqlite3_open(db_path.c_str(), &db);
  ASSERT_EQ(rc, SQLITE_OK) << "Failed to open test db: " << sqlite3_errmsg(db);

  const char *create_sql = R"(
    CREATE TABLE IF NOT EXISTS agents (
        id          TEXT PRIMARY KEY,
        role        TEXT NOT NULL,
        binary_path TEXT NOT NULL,
        manifest    TEXT NOT NULL,
        approved_by TEXT NOT NULL,
        approved_at INTEGER NOT NULL,
        enabled     INTEGER NOT NULL DEFAULT 1
    );
    CREATE TABLE IF NOT EXISTS capabilities (
        agent_id     TEXT NOT NULL REFERENCES agents(id),
        method       TEXT NOT NULL,
        description  TEXT NOT NULL,
        input_schema TEXT NOT NULL,
        cpu_weight   INTEGER,
        memory_mb    INTEGER,
        PRIMARY KEY (agent_id, method)
    );
  )";
  char *err = nullptr;
  rc = sqlite3_exec(db, create_sql, nullptr, nullptr, &err);
  ASSERT_EQ(rc, SQLITE_OK) << "Create tables failed: " << err;
  sqlite3_free(err);

  for (const auto &stmt : insert_statements)
  {
    rc = sqlite3_exec(db, stmt.c_str(), nullptr, nullptr, &err);
    ASSERT_EQ(rc, SQLITE_OK) << "Insert failed: " << err;
    sqlite3_free(err);
  }

  sqlite3_close(db);
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class RegistryTest : public ::testing::Test
{
protected:
  std::string db_path_;

  void SetUp() override
  {
    // Create a unique temporary file name
    char tmp[] = "/tmp/agentos_registry_test_XXXXXX";
    int fd = mkstemp(tmp);
    ASSERT_NE(fd, -1) << "mkstemp failed";
    close(fd);
    db_path_ = tmp;
  }

  void TearDown() override
  {
    std::remove(db_path_.c_str());
  }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(RegistryTest, EmptyDatabase)
{
  // Create an empty database (no agents)
  create_test_db(db_path_, {});

  Registry reg(db_path_);
  EXPECT_EQ(reg.adviser_count(), 0);
  EXPECT_EQ(reg.worker_count(), 0);
  EXPECT_FALSE(reg.find_adviser("research").has_value());
  EXPECT_FALSE(reg.find_worker_for_command("web.search").has_value());
  EXPECT_TRUE(reg.all_command_schemas().empty());
}

TEST_F(RegistryTest, SingleAdviser)
{
  std::vector<std::string> inserts;
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('adviser-1', 'adviser', '/usr/bin/adviser1',
            '{"name":"Adviser One","version":"2.0","domains":["research","coding"]}',
            'human', 1700000000, 1)
  )");

  create_test_db(db_path_, inserts);

  Registry reg(db_path_);
  EXPECT_EQ(reg.adviser_count(), 1);
  EXPECT_EQ(reg.worker_count(), 0);

  auto adviser = reg.find_adviser("research");
  ASSERT_TRUE(adviser.has_value());
  EXPECT_EQ(adviser->id, "adviser-1");
  EXPECT_EQ(adviser->name, "Adviser One");
  EXPECT_EQ(adviser->version, "2.0");
  EXPECT_EQ(adviser->binary_path, "/usr/bin/adviser1");
  ASSERT_EQ(adviser->domains.size(), 2);
  EXPECT_EQ(adviser->domains[0], "research");
  EXPECT_EQ(adviser->domains[1], "coding");

  // Domain not present
  EXPECT_FALSE(reg.find_adviser("finance").has_value());
}

TEST_F(RegistryTest, SingleWorker)
{
  std::vector<std::string> inserts;
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('worker-1', 'worker', '/usr/bin/worker1',
            '{"name":"Worker One","version":"1.5","capabilities":[{"method":"web.search","description":"Search the web","input_schema":{"query":"string"},"output_schema":{"results":"array"}}]}',
            'human', 1700000001, 1)
  )");

  create_test_db(db_path_, inserts);

  Registry reg(db_path_);
  EXPECT_EQ(reg.adviser_count(), 0);
  EXPECT_EQ(reg.worker_count(), 1);

  auto worker = reg.find_worker_for_command("web.search");
  ASSERT_TRUE(worker.has_value());
  EXPECT_EQ(worker->id, "worker-1");
  EXPECT_EQ(worker->name, "Worker One");
  EXPECT_EQ(worker->version, "1.5");
  EXPECT_EQ(worker->binary_path, "/usr/bin/worker1");
  ASSERT_EQ(worker->commands.size(), 1);
  EXPECT_EQ(worker->commands[0].name, "web.search");
  EXPECT_EQ(worker->commands[0].description, "Search the web");

  // Unknown command
  EXPECT_FALSE(reg.find_worker_for_command("unknown").has_value());

  // Schema lookup
  auto schema = reg.get_command_schema("web.search");
  ASSERT_TRUE(schema.has_value());
  EXPECT_EQ(schema->name, "web.search");
  EXPECT_EQ(schema->input.size(), 1);
  EXPECT_EQ(schema->input.at("query").type, "string");
  EXPECT_EQ(schema->output.size(), 1);
  EXPECT_EQ(schema->output.at("results").type, "array");

  // all_command_schemas
  auto all = reg.all_command_schemas();
  ASSERT_EQ(all.size(), 1);
  EXPECT_EQ(all[0].name, "web.search");
}

TEST_F(RegistryTest, MultipleAgents)
{
  std::vector<std::string> inserts;
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('adviser-1', 'adviser', '/usr/bin/adviser1',
            '{"name":"Adviser One","version":"1.0","domains":["research"]}',
            'human', 1700000000, 1)
  )");
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('worker-1', 'worker', '/usr/bin/worker1',
            '{"name":"Worker One","version":"2.0","capabilities":[{"method":"web.search","description":"Search","input_schema":{"q":"string"},"output_schema":{"r":"array"}}]}',
            'human', 1700000001, 1)
  )");
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('worker-2', 'worker', '/usr/bin/worker2',
            '{"name":"Worker Two","version":"3.0","capabilities":[{"method":"file.read","description":"Read file","input_schema":{"path":"string"},"output_schema":{"content":"string"}}]}',
            'human', 1700000002, 1)
  )");

  create_test_db(db_path_, inserts);

  Registry reg(db_path_);
  EXPECT_EQ(reg.adviser_count(), 1);
  EXPECT_EQ(reg.worker_count(), 2);

  // Adviser lookup
  auto adviser = reg.find_adviser("research");
  ASSERT_TRUE(adviser.has_value());
  EXPECT_EQ(adviser->id, "adviser-1");

  // Worker lookups
  auto w1 = reg.find_worker_for_command("web.search");
  ASSERT_TRUE(w1.has_value());
  EXPECT_EQ(w1->id, "worker-1");

  auto w2 = reg.find_worker_for_command("file.read");
  ASSERT_TRUE(w2.has_value());
  EXPECT_EQ(w2->id, "worker-2");

  // Schema lookups
  auto s1 = reg.get_command_schema("web.search");
  ASSERT_TRUE(s1.has_value());
  EXPECT_EQ(s1->name, "web.search");

  auto s2 = reg.get_command_schema("file.read");
  ASSERT_TRUE(s2.has_value());
  EXPECT_EQ(s2->name, "file.read");

  // all_command_schemas
  auto all = reg.all_command_schemas();
  ASSERT_EQ(all.size(), 2);
  // Order not guaranteed, but both names should be present
  std::set<std::string> names;
  for (const auto &s : all)
    names.insert(s.name);
  EXPECT_TRUE(names.count("web.search"));
  EXPECT_TRUE(names.count("file.read"));
}

TEST_F(RegistryTest, DisabledAgentIgnored)
{
  std::vector<std::string> inserts;
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('worker-1', 'worker', '/usr/bin/worker1',
            '{"name":"Worker One","version":"1.0","capabilities":[{"method":"web.search","description":"Search","input_schema":{"q":"string"},"output_schema":{"r":"array"}}]}',
            'human', 1700000000, 0)
  )");

  create_test_db(db_path_, inserts);

  Registry reg(db_path_);
  EXPECT_EQ(reg.worker_count(), 0);
  EXPECT_FALSE(reg.find_worker_for_command("web.search").has_value());
}

TEST_F(RegistryTest, DefaultConstructorEmpty)
{
  Registry reg;
  EXPECT_EQ(reg.adviser_count(), 0);
  EXPECT_EQ(reg.worker_count(), 0);
  EXPECT_FALSE(reg.find_adviser("any").has_value());
  EXPECT_FALSE(reg.find_worker_for_command("any").has_value());
  EXPECT_TRUE(reg.all_command_schemas().empty());
}

TEST_F(RegistryTest, DeprecatedMethodsAreNoOp)
{
  Registry reg;

  RegisteredAdviser adviser;
  adviser.id = "test-adviser";
  adviser.name = "Test";
  adviser.version = "1.0";
  adviser.binary_path = "/bin/test";
  adviser.domains = {"test"};

  RegisteredExecutor worker;
  worker.id = "test-worker";
  worker.name = "TestWorker";
  worker.version = "1.0";
  worker.binary_path = "/bin/testworker";
  CommandSchema cmd;
  cmd.name = "test.command";
  cmd.description = "Test command";
  worker.commands.push_back(cmd);

  // These should not throw and should not affect state
  reg.register_adviser(adviser);
  reg.register_worker(worker);
  reg.remove("nonexistent");

  EXPECT_EQ(reg.adviser_count(), 0);
  EXPECT_EQ(reg.worker_count(), 0);
  EXPECT_FALSE(reg.find_adviser("test").has_value());
  EXPECT_FALSE(reg.find_worker_for_command("test.command").has_value());
}

TEST_F(RegistryTest, LoadFromDbAfterDefaultConstructor)
{
  // Create a database with one worker
  std::vector<std::string> inserts;
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('worker-1', 'worker', '/usr/bin/worker1',
            '{"name":"Worker One","version":"1.0","capabilities":[{"method":"web.search","description":"Search","input_schema":{"q":"string"},"output_schema":{"r":"array"}}]}',
            'human', 1700000000, 1)
  )");
  create_test_db(db_path_, inserts);

  Registry reg;
  EXPECT_EQ(reg.worker_count(), 0);

  reg.load_from_db(db_path_);
  EXPECT_EQ(reg.worker_count(), 1);
  EXPECT_TRUE(reg.find_worker_for_command("web.search").has_value());
}

TEST_F(RegistryTest, LoadFromDbTwiceIgnored)
{
  std::vector<std::string> inserts;
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('worker-1', 'worker', '/usr/bin/worker1',
            '{"name":"Worker One","version":"1.0","capabilities":[{"method":"web.search","description":"Search","input_schema":{"q":"string"},"output_schema":{"r":"array"}}]}',
            'human', 1700000000, 1)
  )");
  create_test_db(db_path_, inserts);

  Registry reg(db_path_);
  EXPECT_EQ(reg.worker_count(), 1);

  // Second load should be ignored (warning logged)
  reg.load_from_db(db_path_);
  EXPECT_EQ(reg.worker_count(), 1);
}
