/**
 * Copyright (C) 2026  HardenedLinux community
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <cstdio>
#include <unistd.h>
#include <string>
#include <vector>

#include "agentos/registry.h"
#include "database/database.h"

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
  Database *db_ = nullptr;

  void SetUp() override
  {
    char tmp[] = "/tmp/agentos_registry_test_XXXXXX";
    int fd = mkstemp(tmp);
    ASSERT_NE(fd, -1) << "mkstemp failed";
    close(fd);
    db_path_ = tmp;
  }

  void TearDown() override
  {
    if (db_)
    {
      db_->close();
      delete db_;
    }
    std::remove(db_path_.c_str());
  }

  // Helper to create a Database object and open it
  Database& open_db()
  {
    db_ = new Database(db_path_);
    EXPECT_TRUE(db_->open());
    return *db_;
  }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(RegistryTest, RegisterAndFindExecutor)
{
  std::vector<std::string> inserts;
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ex-1', 'worker', '/usr/bin/worker1',
            '{"name":"web-search","version":"1.0","capabilities":[{"method":"web.search","description":"test command web.search","input_schema":{},"output_schema":{}},{"method":"web.fetch","description":"test command web.fetch","input_schema":{},"output_schema":{}}]}',
            'human', 1700000000, 1)
  )");

  create_test_db(db_path_, inserts);
  Database &db = open_db();

  Registry reg(db);

  auto ex = reg.find_worker_for_command("web.search");
  ASSERT_TRUE(ex.has_value());
  EXPECT_EQ(ex->id, "ex-1");
}

TEST_F(RegistryTest, UnknownCommandReturnsNullopt)
{
  create_test_db(db_path_, {});
  Database &db = open_db();
  Registry reg(db);
  EXPECT_FALSE(reg.find_worker_for_command("nonexistent.cmd").has_value());
}

TEST_F(RegistryTest, RegisterAndFindAgent)
{
  std::vector<std::string> inserts;
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ag-1', 'adviser', '/usr/bin/adviser1',
            '{"name":"research-agent","version":"1.0","domains":["research","general"]}',
            'human', 1700000000, 1)
  )");

  create_test_db(db_path_, inserts);
  Database &db = open_db();

  Registry reg(db);

  auto ag = reg.find_adviser("research");
  ASSERT_TRUE(ag.has_value());
  EXPECT_EQ(ag->id, "ag-1");
}

TEST_F(RegistryTest, UnknownDomainReturnsNullopt)
{
  create_test_db(db_path_, {});
  Database &db = open_db();
  Registry reg(db);
  EXPECT_FALSE(reg.find_adviser("coding").has_value());
}

TEST_F(RegistryTest, RemoveExecutorUnregistersCommands)
{
  std::vector<std::string> inserts;
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ex-1', 'worker', '/usr/bin/worker1',
            '{"name":"web-search","version":"1.0","capabilities":[{"method":"web.search","description":"test command web.search","input_schema":{},"output_schema":{}}]}',
            'human', 1700000000, 1)
  )");

  create_test_db(db_path_, inserts);
  Database &db = open_db();

  Registry reg(db);

  ASSERT_TRUE(reg.find_worker_for_command("web.search").has_value());

  // remove is deprecated and no-op; the command should still be found
  reg.remove("ex-1");
  // Since static catalog cannot be modified at runtime, the command remains
  EXPECT_TRUE(reg.find_worker_for_command("web.search").has_value());
}

TEST_F(RegistryTest, AllCommandSchemas)
{
  std::vector<std::string> inserts;
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ex-1', 'worker', '/usr/bin/worker1',
            '{"name":"web","version":"1.0","capabilities":[{"method":"web.search","description":"test command web.search","input_schema":{},"output_schema":{}},{"method":"web.fetch","description":"test command web.fetch","input_schema":{},"output_schema":{}}]}',
            'human', 1700000000, 1)
  )");
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ex-2', 'worker', '/usr/bin/worker2',
            '{"name":"file","version":"1.0","capabilities":[{"method":"file.write","description":"test command file.write","input_schema":{},"output_schema":{}},{"method":"file.read","description":"test command file.read","input_schema":{},"output_schema":{}}]}',
            'human', 1700000000, 1)
  )");

  create_test_db(db_path_, inserts);
  Database &db = open_db();

  Registry reg(db);

  auto schemas = reg.all_command_schemas();
  EXPECT_EQ(schemas.size(), 4u);
}

TEST_F(RegistryTest, Counts)
{
  std::vector<std::string> inserts;
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ag-1', 'adviser', '/usr/bin/adviser1',
            '{"name":"adviser","version":"1.0","domains":["general"]}',
            'human', 1700000000, 1)
  )");
  inserts.push_back(R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ex-1', 'worker', '/usr/bin/worker1',
            '{"name":"exec","version":"1.0","capabilities":[{"method":"cmd.run","description":"test command cmd.run","input_schema":{},"output_schema":{}}]}',
            'human', 1700000000, 1)
  )");

  create_test_db(db_path_, inserts);
  Database &db = open_db();

  Registry reg(db);
  EXPECT_EQ(reg.adviser_count(), 1u);
  EXPECT_EQ(reg.worker_count(), 1u);
}
