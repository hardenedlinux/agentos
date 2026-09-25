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
#include "agentos/database.h"
#include <filesystem>
#include <gtest/gtest.h>
#include <sqlite3.h>

using namespace agentos;

class DatabaseTest : public ::testing::Test
{
protected:
  std::string db_path;
  std::unique_ptr<Database> db;

  void SetUp () override
  {
    db_path = "/tmp/agentos_test_db_" + std::to_string (getpid ()) + ".db";
    db = std::make_unique<Database> (db_path);
    ASSERT_TRUE (db->open ());
  }

  void TearDown () override
  {
    db->close ();
    std::filesystem::remove (db_path);
  }
};

TEST_F (DatabaseTest, OpenAndClose)
{
  EXPECT_TRUE (db->is_open ());
  db->close ();
  EXPECT_FALSE (db->is_open ());
}

TEST_F (DatabaseTest, StoreAndLoadJob)
{
  Task task;
  task.id = "job1";
  task.goal = "test goal";
  task.input_json = R"({"key":"value"})";
  db->store_job (task);
  db->update_job_phase ("job1", "planning");

  // The original job table (preserved for compatibility) has no per-job
  // plan_json column — ADR-022 moved plan/step persistence to the tasks
  // table (store_pipeline_task / load_pipeline_steps_for_job, keyed by
  // PipelinePlanStep) instead of a single JSON blob per job. What this
  // table actually offers is phase tracking + crash-recovery listing,
  // so that's what this test verifies.
  auto jobs = db->resume_in_flight ();
  ASSERT_EQ (jobs.size (), 1);
  EXPECT_EQ (jobs[0].job_id, "job1");
}

TEST_F (DatabaseTest, ResumeInFlight)
{
  Task task;
  task.id = "job1";
  task.goal = "test";
  db->store_job (task);
  auto jobs = db->resume_in_flight ();
  ASSERT_EQ (jobs.size (), 1);
  EXPECT_EQ (jobs[0].job_id, "job1");
}

TEST_F (DatabaseTest, ResumeInFlightNone)
{
  auto jobs = db->resume_in_flight ();
  EXPECT_TRUE (jobs.empty ());
}

// ---------------------------------------------------------------------------
// insert_agent used to be INSERT OR REPLACE, which hardcodes enabled=1 in
// its VALUES clause. On a primary-key conflict SQLite deletes then
// re-inserts the row, so re-registering an id that had been disabled or
// revoked silently reset it back to enabled — undoing set_worker_enabled()/
// revoke_worker() with no warning. This pins down the fix: re-registering
// an existing id must preserve its current `enabled` value while still
// updating everything else (manifest/description/etc).
// ---------------------------------------------------------------------------
TEST_F (DatabaseTest, InsertAgentPreservesEnabledStateAcrossReRegistration)
{
  db->insert_agent ("w1", "worker", "/bin/w1", "manifest v1", "desc v1",
                    "human");
  db->set_worker_enabled ("w1", false);

  // Re-register the same id — as Forge/adviser.register/worker.register
  // would on a legitimate re-registration.
  db->insert_agent ("w1", "worker", "/bin/w1", "manifest v2", "desc v2",
                    "human");

  sqlite3 *raw = nullptr;
  ASSERT_EQ (sqlite3_open (db_path.c_str (), &raw), SQLITE_OK);
  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (
               raw, "SELECT enabled, manifest, description FROM agents WHERE id = ?",
               -1, &stmt, nullptr),
            SQLITE_OK);
  sqlite3_bind_text (stmt, 1, "w1", -1, SQLITE_TRANSIENT);
  ASSERT_EQ (sqlite3_step (stmt), SQLITE_ROW);

  // The disabled state must survive the re-registration...
  EXPECT_EQ (sqlite3_column_int (stmt, 0), 0);
  // ...while the rest of the row still reflects the new registration.
  EXPECT_STREQ (reinterpret_cast<const char *> (sqlite3_column_text (stmt, 1)),
               "manifest v2");
  EXPECT_STREQ (reinterpret_cast<const char *> (sqlite3_column_text (stmt, 2)),
               "desc v2");

  sqlite3_finalize (stmt);
  sqlite3_close (raw);
}
