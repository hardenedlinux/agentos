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

#include "agentos/verifier.h"
#include "agentos/registry.h"
#include "database/database.hpp"

using namespace agentos;

// ---------------------------------------------------------------------------
// Helper: create a temporary SQLite database with the ADR-007 schema and
// populate it with a worker that has a web.search command.
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
class VerifierTest : public ::testing::Test
{
protected:
  std::string db_path_;
  Database *db_ = nullptr;

  void SetUp() override
  {
    char tmp[] = "/tmp/agentos_verifier_test_XXXXXX";
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
// Helper: create a registry with a web.search command
// ---------------------------------------------------------------------------
static Registry make_registry(Database &db)
{
  sqlite3 *handle = db.db_handle();
  EXPECT_NE(handle, nullptr);

  // Create tables if they don't exist
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
  int rc = sqlite3_exec(handle, create_sql, nullptr, nullptr, &err);
  EXPECT_EQ(rc, SQLITE_OK) << "Create tables failed: " << err;
  sqlite3_free(err);

  // Insert the worker
  const char *insert_sql = R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ex-1', 'worker', '/usr/bin/worker1',
            '{"name":"web-search","version":"1.0","capabilities":[{"method":"web.search","description":"Searches the web","input_schema":{"query":{"type":"string","required":true},"max_results":{"type":"integer","required":false}},"output_schema":{}}]}',
            'human', 1700000000, 1)
  )";
  rc = sqlite3_exec(handle, insert_sql, nullptr, nullptr, &err);
  EXPECT_EQ(rc, SQLITE_OK) << "Insert failed: " << err;
  sqlite3_free(err);

  return Registry(db);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(VerifierTest, ValidPlanPasses)
{
  Database &db = open_db();
  Registry reg = make_registry(db);
  Verifier v(reg);

  Plan plan;
  plan.task_id = "t1";
  plan.steps.push_back({ "s1", "web.search", {{"query", "test"}}, {} });

  auto r = v.verify(plan);
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.errors.empty());
}

TEST_F(VerifierTest, UnknownCommandFails)
{
  Database &db = open_db();
  Registry reg = make_registry(db);
  Verifier v(reg);

  Plan plan;
  plan.task_id = "t1";
  plan.steps.push_back({ "s1", "nonexistent.cmd", {}, {} });

  auto r = v.verify(plan);
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(r.errors.empty());
}

TEST_F(VerifierTest, MissingRequiredArgFails)
{
  Database &db = open_db();
  Registry reg = make_registry(db);
  Verifier v(reg);

  Plan plan;
  plan.task_id = "t1";
  // 'query' is required but not provided
  plan.steps.push_back({ "s1", "web.search", {}, {} });

  auto r = v.verify(plan);
  EXPECT_FALSE(r.ok);
}

TEST_F(VerifierTest, InvalidDependencyFails)
{
  Database &db = open_db();
  Registry reg = make_registry(db);
  Verifier v(reg);

  Plan plan;
  plan.task_id = "t1";
  plan.steps.push_back({ "s1", "web.search", {{"query","x"}}, {"nonexistent_step"} });

  auto r = v.verify(plan);
  EXPECT_FALSE(r.ok);
}

TEST_F(VerifierTest, ValidVariableRefPasses)
{
  Database &db = open_db();
  Registry reg = make_registry(db);
  Verifier v(reg);

  Plan plan;
  plan.task_id = "t1";
  plan.steps.push_back({ "s1", "web.search", {{"query", "initial"}}, {} });
  plan.steps.push_back({ "s2", "web.search", {{"query", "{{s1.result}}"}}, {"s1"} });

  auto r = v.verify(plan);
  EXPECT_TRUE(r.ok);
}

TEST_F(VerifierTest, InvalidVariableRefFails)
{
  Database &db = open_db();
  Registry reg = make_registry(db);
  Verifier v(reg);

  Plan plan;
  plan.task_id = "t1";
  // References a step that doesn't exist
  plan.steps.push_back({ "s1", "web.search", {{"query", "{{ghost_step.result}}"}}, {} });

  auto r = v.verify(plan);
  EXPECT_FALSE(r.ok);
}
