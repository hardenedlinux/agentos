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

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "agentos/database.h"
#include "agentos/forge_pipeline_job.h"
#include "agentos/registry.h"

using namespace agentos;

// ---------------------------------------------------------------------------
// Helper: create a temporary SQLite database with the ADR-007 schema and
// populate it with the given agents.
// ---------------------------------------------------------------------------
static void create_test_db (const std::string &db_path,
                            const std::vector<std::string> &insert_statements)
{
  sqlite3 *db = nullptr;
  int rc = sqlite3_open (db_path.c_str (), &db);
  ASSERT_EQ (rc, SQLITE_OK)
    << "Failed to open test db: " << sqlite3_errmsg (db);

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
  rc = sqlite3_exec (db, create_sql, nullptr, nullptr, &err);
  ASSERT_EQ (rc, SQLITE_OK) << "Create tables failed: " << err;
  sqlite3_free (err);

  for (const auto &stmt : insert_statements)
  {
    rc = sqlite3_exec (db, stmt.c_str (), nullptr, nullptr, &err);
    ASSERT_EQ (rc, SQLITE_OK) << "Insert failed: " << err;
    sqlite3_free (err);
  }

  sqlite3_close (db);
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class RegistryTest : public ::testing::Test
{
protected:
  std::string db_path_;
  Database *db_ = nullptr;
  std::string agentos_home_dir_;
  bool had_agentos_home_ = false;
  std::string prev_agentos_home_;
  spdlog::level::level_enum prev_log_level_ = spdlog::level::info;

  void SetUp () override
  {
    char tmp[] = "/tmp/agentos_registry_test_XXXXXX";
    int fd = mkstemp (tmp);
    ASSERT_NE (fd, -1) << "mkstemp failed";
    close (fd);
    db_path_ = tmp;

    // finalize_worker_promotion() writes under agentos_home()/workers/<id>/
    // (manifest.json, worker.py path). Sandbox AGENTOS_HOME per test so
    // these tests never touch the real ~/.agentos and each test gets a
    // clean directory.
    char home_tmp[] = "/tmp/agentos_registry_test_home_XXXXXX";
    char *home_dir = mkdtemp (home_tmp);
    ASSERT_NE (home_dir, nullptr) << "mkdtemp failed";
    agentos_home_dir_ = home_dir;

    if (const char *existing = std::getenv ("AGENTOS_HOME"))
    {
      had_agentos_home_ = true;
      prev_agentos_home_ = existing;
    }
    setenv ("AGENTOS_HOME", agentos_home_dir_.c_str (), 1);

    // Database::open() runs seed_builtin_advisers(), which correctly
    // reports missing manifest.toml/skill.md for planning/code-writer/
    // code-reviewer under this deliberately empty sandbox — these tests
    // don't seed those files on purpose (doing so would register real
    // adviser rows and inflate adviser_count() in other tests). The
    // check is accurate, not a bug; just silence log output for the
    // duration of the test rather than fabricate files to quiet it.
    prev_log_level_ = spdlog::get_level ();
    spdlog::set_level (spdlog::level::off);
  }

  void TearDown () override
  {
    if (db_)
    {
      db_->close ();
      delete db_;
    }
    std::remove (db_path_.c_str ());

    std::error_code ec;
    std::filesystem::remove_all (agentos_home_dir_, ec);

    if (had_agentos_home_)
      setenv ("AGENTOS_HOME", prev_agentos_home_.c_str (), 1);
    else
      unsetenv ("AGENTOS_HOME");

    spdlog::set_level (prev_log_level_);
  }

  // Helper to create a Database object and open it
  Database &open_db ()
  {
    db_ = new Database (db_path_);
    EXPECT_TRUE (db_->open ());
    return *db_;
  }

  // Raw-sqlite row count against db_path_, bypassing the Database/Registry
  // layer, so assertions about what got persisted are independent of
  // whatever in-memory bookkeeping finalize_worker_promotion also does.
  int count_rows (const std::string &sql, const std::string &bind_value)
  {
    sqlite3 *raw = nullptr;
    EXPECT_EQ (sqlite3_open (db_path_.c_str (), &raw), SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    EXPECT_EQ (sqlite3_prepare_v2 (raw, sql.c_str (), -1, &stmt, nullptr),
               SQLITE_OK);
    sqlite3_bind_text (stmt, 1, bind_value.c_str (), -1, SQLITE_TRANSIENT);
    int count = -1;
    if (sqlite3_step (stmt) == SQLITE_ROW)
      count = sqlite3_column_int (stmt, 0);
    sqlite3_finalize (stmt);
    sqlite3_close (raw);
    return count;
  }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F (RegistryTest, RegisterAndFindExecutor)
{
  std::vector<std::string> inserts;
  inserts.push_back (R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ex-1', 'worker', '/usr/bin/worker1',
            '{"name":"web-search","version":"1.0","capabilities":[{"method":"web.search","description":"test command web.search","input_schema":{},"output_schema":{}},{"method":"web.fetch","description":"test command web.fetch","input_schema":{},"output_schema":{}}]}',
            'human', 1700000000, 1)
  )");

  create_test_db (db_path_, inserts);
  Database &db = open_db ();

  Registry reg;
  reg.init (db);

  auto ex = reg.find_worker_for_command ("web.search");
  ASSERT_TRUE (ex.has_value ());
  EXPECT_EQ (ex->id, "ex-1");
}

TEST_F (RegistryTest, UnknownCommandReturnsNullopt)
{
  create_test_db (db_path_, {});
  Database &db = open_db ();
  Registry reg;
  reg.init (db);
  EXPECT_FALSE (reg.find_worker_for_command ("nonexistent.cmd").has_value ());
}

TEST_F (RegistryTest, RegisterAndFindAgent)
{
  std::vector<std::string> inserts;
  // Advisers ship manifest.toml (ADR-018) — parse_manifest dispatches
  // role=="adviser" through the TOML parser, not JSON. This fixture used
  // to be JSON and silently failed to parse after that dispatch was
  // fixed; keeping it in sync here.
  inserts.push_back (R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ag-1', 'adviser', '/usr/bin/adviser1',
            '[meta]
id = "ag-1"
version = "1.0"
domains = ["research", "general"]
',
            'human', 1700000000, 1)
  )");

  create_test_db (db_path_, inserts);
  Database &db = open_db ();

  Registry reg;
  reg.init (db);

  auto ag_vec = reg.find_advisers_by_domain ({"research"});
  ASSERT_FALSE (ag_vec.empty ());
  const auto& ag = ag_vec[0];
  EXPECT_EQ (ag.id, "ag-1");
}

TEST_F (RegistryTest, UnknownDomainReturnsNullopt)
{
  create_test_db (db_path_, {});
  Database &db = open_db ();
  Registry reg;
  reg.init (db);
  auto ag_vec = reg.find_advisers_by_domain ({"coding"});
  EXPECT_TRUE (ag_vec.empty ());
}

TEST_F (RegistryTest, RemoveExecutorUnregistersCommands)
{
  std::vector<std::string> inserts;
  inserts.push_back (R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ex-1', 'worker', '/usr/bin/worker1',
            '{"name":"web-search","version":"1.0","capabilities":[{"method":"web.search","description":"test command web.search","input_schema":{},"output_schema":{}}]}',
            'human', 1700000000, 1)
  )");

  create_test_db (db_path_, inserts);
  Database &db = open_db ();

  Registry reg;
  reg.init (db);

  ASSERT_TRUE (reg.find_worker_for_command ("web.search").has_value ());

  // remove is deprecated and no-op; the command should still be found
  reg.remove ("ex-1");
  // Since static catalog cannot be modified at runtime, the command remains
  EXPECT_TRUE (reg.find_worker_for_command ("web.search").has_value ());
}

TEST_F (RegistryTest, AllCommandSchemas)
{
  std::vector<std::string> inserts;
  inserts.push_back (R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ex-1', 'worker', '/usr/bin/worker1',
            '{"name":"web","version":"1.0","capabilities":[{"method":"web.search","description":"test command web.search","input_schema":{},"output_schema":{}},{"method":"web.fetch","description":"test command web.fetch","input_schema":{},"output_schema":{}}]}',
            'human', 1700000000, 1)
  )");
  inserts.push_back (R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ex-2', 'worker', '/usr/bin/worker2',
            '{"name":"file","version":"1.0","capabilities":[{"method":"file.write","description":"test command file.write","input_schema":{},"output_schema":{}},{"method":"file.read","description":"test command file.read","input_schema":{},"output_schema":{}}]}',
            'human', 1700000000, 1)
  )");

  create_test_db (db_path_, inserts);
  Database &db = open_db ();

  Registry reg;
  reg.init (db);

  auto schemas = reg.all_command_schemas ();
  EXPECT_EQ (schemas.size (), 4u);
}

TEST_F (RegistryTest, Counts)
{
  std::vector<std::string> inserts;
  // Advisers ship manifest.toml (ADR-018) — see note on RegisterAndFindAgent.
  inserts.push_back (R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ag-1', 'adviser', '/usr/bin/adviser1',
            '[meta]
id = "ag-1"
version = "1.0"
domains = ["general"]
',
            'human', 1700000000, 1)
  )");
  inserts.push_back (R"(
    INSERT INTO agents (id, role, binary_path, manifest, approved_by, approved_at, enabled)
    VALUES ('ex-1', 'worker', '/usr/bin/worker1',
            '{"name":"exec","version":"1.0","capabilities":[{"method":"cmd.run","description":"test command cmd.run","input_schema":{},"output_schema":{}}]}',
            'human', 1700000000, 1)
  )");

  create_test_db (db_path_, inserts);
  Database &db = open_db ();

  Registry reg;
  reg.init (db);
  EXPECT_EQ (reg.adviser_count (), 1u);
  EXPECT_EQ (reg.worker_count (), 1u);
}

// ---------------------------------------------------------------------------
// finalize_worker_promotion: atomic promotion (regression tests for the
// empty-shell-worker bug — a Forge-generated worker whose capability
// method format failed validation used to still get an `agents` row
// inserted, just with the bad capability silently skipped).
// ---------------------------------------------------------------------------

TEST_F (RegistryTest, FinalizeWorkerPromotionSuccessRegistersWorker)
{
  create_test_db (db_path_, {});
  Database &db = open_db ();

  Registry reg;
  reg.init (db);

  ForgePipelineJob job;
  job.id = "forge-ok-1";

  const std::string capability_json = R"({
    "name": "forge-ok-1",
    "version": "1.0",
    "capabilities": [
      {"method": "test.run", "description": "run a test", "input_schema": {}}
    ]
  })";

  const bool ok = reg.finalize_worker_promotion (job, "print('worker')",
                                                 capability_json, db);
  EXPECT_TRUE (ok);

  // In-memory registry sees the new command.
  auto worker = reg.find_worker_for_command ("test.run");
  ASSERT_TRUE (worker.has_value ());
  EXPECT_EQ (worker->id, "forge-ok-1");

  // DB: agent row and capability row both landed.
  EXPECT_EQ (count_rows ("SELECT COUNT(*) FROM agents WHERE id = ?",
                        job.id),
            1);
  EXPECT_EQ (
    count_rows ("SELECT COUNT(*) FROM capabilities WHERE agent_id = ?",
               job.id),
    1);

  // manifest.json was written to disk.
  auto manifest_path = std::filesystem::path (agentos_home_dir_) / "workers"
                      / job.id / "manifest.json";
  EXPECT_TRUE (std::filesystem::exists (manifest_path));
}

TEST_F (RegistryTest, FinalizeWorkerPromotionRejectsInvalidMethodAtomically)
{
  create_test_db (db_path_, {});
  Database &db = open_db ();

  Registry reg;
  reg.init (db);

  ForgePipelineJob job;
  job.id = "forge-bad-1";

  // First capability is well-formed; second violates ADR-031's
  // namespace.verb format (no dot, uppercase letters). The whole
  // promotion must be rejected — not just the bad entry skipped.
  const std::string capability_json = R"({
    "name": "forge-bad-1",
    "version": "1.0",
    "capabilities": [
      {"method": "test.run", "description": "valid one", "input_schema": {}},
      {"method": "BadMethodName", "description": "invalid", "input_schema": {}}
    ]
  })";

  const bool ok = reg.finalize_worker_promotion (job, "print('worker')",
                                                 capability_json, db);
  EXPECT_FALSE (ok);

  // Regression guard: even the VALID capability from the same manifest
  // must not have been registered in memory. This is the empty-shell
  // worker bug — no partial registration allowed.
  EXPECT_FALSE (reg.find_worker_for_command ("test.run").has_value ());

  // DB: no agent row and no capability rows for this job at all.
  EXPECT_EQ (count_rows ("SELECT COUNT(*) FROM agents WHERE id = ?",
                        job.id),
            0);
  EXPECT_EQ (
    count_rows ("SELECT COUNT(*) FROM capabilities WHERE agent_id = ?",
               job.id),
    0);

  // manifest.json must not have been written either.
  auto manifest_path = std::filesystem::path (agentos_home_dir_) / "workers"
                      / job.id / "manifest.json";
  EXPECT_FALSE (std::filesystem::exists (manifest_path));
}
