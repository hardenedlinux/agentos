/**
 * Unit tests for ADR-022 pipeline step persistence (Database layer).
 * Covers:
 *   - store_pipeline_task
 *   - load_step_result
 *   - Schema migration for new task columns
 */

#include <gtest/gtest.h>

#include "agentos/database.h"
#include "agentos/home_init.h"
#include "agentos/types.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// -------------------------------------------------------------------------
// Test fixture – creates a temporary database file and opens it.
// -------------------------------------------------------------------------

class DatabasePipelineTest : public ::testing::Test
{
protected:
  void SetUp () override
  {
    char tmpl[] = "/tmp/agentos_pipeline_test_XXXXXX";
    int fd = mkstemp (tmpl);
    ASSERT_NE (fd, -1);
    close (fd);
    db_path_ = tmpl;

    db_ = std::make_unique<agentos::Database> (db_path_);
    ASSERT_TRUE (db_->open ()) << "Failed to open test database";
  }

  void TearDown () override
  {
    db_->close ();
    db_.reset ();
    std::remove (db_path_.c_str ());
  }

  std::string db_path_;
  std::unique_ptr<agentos::Database> db_;
};

// -------------------------------------------------------------------------
// Helper: sqlite3_column_text() returns nullptr for SQL NULL columns.
// std::string(nullptr) is undefined behavior / throws in libstdc++ — this
// wraps the cast so a NULL column reads as "" instead of crashing the test.
// -------------------------------------------------------------------------
static std::string column_text_or_empty (sqlite3_stmt *stmt, int col)
{
  const unsigned char *text = sqlite3_column_text (stmt, col);
  return text ? std::string (reinterpret_cast<const char *> (text))
              : std::string ();
}

// -------------------------------------------------------------------------
// Helper: execute a raw SQL string on the test database.
// -------------------------------------------------------------------------
static void exec_sql (sqlite3 *handle, const char *sql)
{
  char *err = nullptr;
  sqlite3_exec (handle, sql, nullptr, nullptr, &err);
  if (err)
  {
    FAIL () << "exec_sql failed: " << err;
    sqlite3_free (err);
  }
}

// -------------------------------------------------------------------------
// store_pipeline_task – basic insert
// -------------------------------------------------------------------------
TEST_F (DatabasePipelineTest, StorePipelineTask_InsertNewStep)
{
  auto job_id = agentos::TaskId ("job-01");

  // tasks has a FOREIGN KEY on jobs(id) — insert parent row first.
  agentos::Task task;
  task.id = job_id;
  task.goal = "test goal";
  task.input_json = "{}";
  db_->store_job (task);

  agentos::PipelinePlanStep step;
  step.id = "step-1";
  step.command = "extract.text";
  step.description = "Extract plain text from PDF";
  step.params["file"] = "document.pdf";

  const int order = 0;

  db_->store_pipeline_task (job_id, step, order);

  // Verify the row exists in the tasks table.
  sqlite3 *h = db_->db_handle ();
  ASSERT_NE (h, nullptr);

  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (h,
                                 "SELECT job_id, agent_id, method, params, "
                                 "description, step_order, status, result "
                                 "FROM tasks WHERE id = ?",
                                 -1, &stmt, nullptr),
             SQLITE_OK);
  sqlite3_bind_text (stmt, 1, step.id.c_str (), -1, SQLITE_TRANSIENT);
  ASSERT_EQ (sqlite3_step (stmt), SQLITE_ROW);

  EXPECT_EQ (column_text_or_empty (stmt, 0), job_id.value ());
  // agent_id (col 1) is intentionally left NULL — store_pipeline_task does
  // not write it (dead field, see backlog). NULL reads as "" via the helper
  // above, matching the original intent of this assertion.
  EXPECT_EQ (column_text_or_empty (stmt, 1), ""); // agent_id remains empty
  EXPECT_EQ (column_text_or_empty (stmt, 2), step.command);
  // params is a JSON object; we only check that the string is not empty.
  EXPECT_GT (sqlite3_column_bytes (stmt, 3), 0);
  EXPECT_EQ (column_text_or_empty (stmt, 4), step.description);
  EXPECT_EQ (sqlite3_column_int (stmt, 5), order);
  EXPECT_EQ (column_text_or_empty (stmt, 6),
             "pending"); // status set to 'pending' by store_pipeline_task
  EXPECT_EQ (sqlite3_column_type (stmt, 7), SQLITE_NULL); // result still null

  sqlite3_finalize (stmt);
}

// -------------------------------------------------------------------------
// load_step_result – retrieves previously persisted result
// -------------------------------------------------------------------------
TEST_F (DatabasePipelineTest, LoadStepResult_ReturnsStoredResult)
{
  auto job_id = agentos::TaskId ("job-load");

  agentos::Task task;
  task.id = job_id;
  task.goal = "test goal";
  task.input_json = "{}";
  db_->store_job (task);

  agentos::PipelinePlanStep step;
  step.id = "load-step";
  step.command = "transform.map";
  step.description = "Map transformation";
  const int order = 1;

  db_->store_pipeline_task (job_id, step, order);

  // Manually set the result column to simulate step completion.
  const std::string expected_result = R"({"status":"ok","count":42})";
  sqlite3 *h = db_->db_handle ();

  {
    sqlite3_stmt *stmt = nullptr;
    ASSERT_EQ (sqlite3_prepare_v2 (h,
                                   "UPDATE tasks SET result = ? WHERE job_id = ? AND id = ?",
                                   -1, &stmt, nullptr),
               SQLITE_OK);
    sqlite3_bind_text (stmt, 1, expected_result.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, job_id.value ().c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, step.id.c_str (), -1, SQLITE_TRANSIENT);
    ASSERT_EQ (sqlite3_step (stmt), SQLITE_DONE);
    sqlite3_finalize (stmt);
  }

  const std::string loaded = db_->load_step_result (job_id.value (), step.id);
  EXPECT_EQ (loaded, expected_result);
}

// -------------------------------------------------------------------------
// load_step_result – missing step returns empty string
// -------------------------------------------------------------------------
TEST_F (DatabasePipelineTest, LoadStepResult_MissingStepReturnsEmpty)
{
  EXPECT_EQ (db_->load_step_result ("no-such-job", "nonexistent"), "");
}

// -------------------------------------------------------------------------
// ADR-031 §13.1: step ids are plan-local labels. Two jobs using the same
// label must not overwrite or read each other's step rows.
// -------------------------------------------------------------------------
TEST_F (DatabasePipelineTest, SameStepLabelInTwoJobs_IsIsolated)
{
  auto job_a = agentos::TaskId ("job-a");
  auto job_b = agentos::TaskId ("job-b");
  for (const auto &id : {job_a, job_b})
  {
    agentos::Task task;
    task.id = id;
    task.goal = "g";
    task.input_json = "{}";
    db_->store_job (task);
  }

  agentos::PipelinePlanStep step;
  step.id = "step-0";
  step.command = "x.y";
  step.description = "d";
  db_->store_pipeline_task (job_a, step, 0);
  db_->store_pipeline_task (job_b, step, 0);

  db_->update_step_result (job_a.value (), "step-0", R"({"owner":"a"})");
  db_->update_step_result (job_b.value (), "step-0", R"({"owner":"b"})");

  EXPECT_EQ (db_->load_step_result (job_a.value (), "step-0"),
             R"({"owner":"a"})");
  EXPECT_EQ (db_->load_step_result (job_b.value (), "step-0"),
             R"({"owner":"b"})");
  EXPECT_EQ (db_->load_pipeline_steps_for_job (job_a.value ()).size (), 0u)
    << "job-a's only step is done; job-b's pending row must not leak in";
}

// ADR-039 §H2a: jobs.state_seq advances with every change to the job's
// job.status shape (job row and its steps); moving notified_seq does not.
TEST_F (DatabasePipelineTest, OutboxWatermark_StateSeqAndNotifiedSeq)
{
  agentos::Task task;
  task.id = agentos::TaskId ("job-w");
  task.goal = "g";
  task.input_json = "{}";
  task.user_id = "0";
  db_->store_job (task);

  const auto s0 = db_->job_state_seq ("job-w");
  EXPECT_GT (s0, 0) << "a new job is a state not yet in the outbox";
  EXPECT_EQ (db_->job_state_seq ("missing"), -1);
  auto pending = db_->jobs_pending_notify ();
  ASSERT_EQ (pending.size (), 1u);
  EXPECT_EQ (pending[0], "job-w");

  ASSERT_TRUE (db_->mark_job_notified ("job-w", s0));
  EXPECT_EQ (db_->job_state_seq ("job-w"), s0)
    << "advancing the watermark is not a job.status change";
  EXPECT_TRUE (db_->jobs_pending_notify ().empty ());

  agentos::PipelinePlanStep step;
  step.id = "step-0";
  step.command = "x.y";
  step.description = "d";
  db_->store_pipeline_task (task.id, step, 0);
  const auto s1 = db_->job_state_seq ("job-w");
  EXPECT_GT (s1, s0) << "step insert";

  db_->update_step_status ("job-w", "step-0", "running");
  const auto s2 = db_->job_state_seq ("job-w");
  EXPECT_GT (s2, s1) << "step status";

  db_->update_job_phase (task.id, "executing");
  const auto s3 = db_->job_state_seq ("job-w");
  EXPECT_GT (s3, s2) << "job phase";

  ASSERT_EQ (db_->jobs_pending_notify ().size (), 1u);

  // The watermark never moves backwards.
  ASSERT_TRUE (db_->mark_job_notified ("job-w", s3));
  ASSERT_TRUE (db_->mark_job_notified ("job-w", s0));
  EXPECT_TRUE (db_->jobs_pending_notify ().empty ());
}

// -------------------------------------------------------------------------
// Schema migration – tasks table has the new columns
// -------------------------------------------------------------------------
TEST_F (DatabasePipelineTest, TasksTableHasNewColumns)
{
  sqlite3 *h = db_->db_handle ();
  ASSERT_NE (h, nullptr);

  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (
    sqlite3_prepare_v2 (h, "PRAGMA table_info('tasks')", -1, &stmt, nullptr),
    SQLITE_OK);

  bool has_result = false;
  bool has_description = false;
  bool has_step_order = false;
  bool has_started_at = false;
  bool has_completed_at = false;

  while (sqlite3_step (stmt) == SQLITE_ROW)
  {
    const char *name
      = reinterpret_cast<const char *> (sqlite3_column_text (stmt, 1));
    if (!name)
      continue;
    std::string col (name);
    if (col == "result")
      has_result = true;
    else if (col == "description")
      has_description = true;
    else if (col == "step_order")
      has_step_order = true;
    else if (col == "started_at")
      has_started_at = true;
    else if (col == "completed_at")
      has_completed_at = true;
  }
  sqlite3_finalize (stmt);

  EXPECT_TRUE (has_result) << "tasks.result missing after migration";
  EXPECT_TRUE (has_description) << "tasks.description missing";
  EXPECT_TRUE (has_step_order) << "tasks.step_order missing";
  EXPECT_TRUE (has_started_at) << "tasks.started_at missing";
  EXPECT_TRUE (has_completed_at) << "tasks.completed_at missing";
}

// -------------------------------------------------------------------------
// store_pipeline_task – overwrites existing step (id conflict)
// -------------------------------------------------------------------------
TEST_F (DatabasePipelineTest, StorePipelineTask_Overwrite)
{
  auto job_id = agentos::TaskId ("j-overwrite");

  agentos::Task task;
  task.id = job_id;
  task.goal = "test goal";
  task.input_json = "{}";
  db_->store_job (task);

  agentos::PipelinePlanStep step1;
  step1.id = "step-ow";
  step1.command = "cmd.one";
  step1.description = "original description";
  step1.params["key"] = "val1";

  db_->store_pipeline_task (job_id, step1, 0);

  // Overwrite with a different description and order
  agentos::PipelinePlanStep step2;
  step2.id = "step-ow";
  step2.command = "cmd.two";
  step2.description = "updated description";
  step2.params["key"] = "val2";
  db_->store_pipeline_task (job_id, step2, 42);

  sqlite3 *h = db_->db_handle ();
  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (h,
                                 "SELECT description, step_order, method "
                                 "FROM tasks WHERE id = ?",
                                 -1, &stmt, nullptr),
             SQLITE_OK);
  sqlite3_bind_text (stmt, 1, step2.id.c_str (), -1, SQLITE_TRANSIENT);
  ASSERT_EQ (sqlite3_step (stmt), SQLITE_ROW);

  EXPECT_EQ (column_text_or_empty (stmt, 0), step2.description);
  EXPECT_EQ (sqlite3_column_int (stmt, 1), 42);
  EXPECT_EQ (column_text_or_empty (stmt, 2), step2.command);
  sqlite3_finalize (stmt);
}

// -------------------------------------------------------------------------
// ADR-031 §13.1 migration: a database created with the old schema (tasks
// keyed by id alone) is rebuilt with PRIMARY KEY (job_id, id), keeping rows.
// -------------------------------------------------------------------------
TEST (DatabaseTasksMigrationTest, OldSingleColumnPrimaryKeyIsRebuilt)
{
  char tmpl[] = "/tmp/agentos_tasks_migration_XXXXXX";
  int fd = mkstemp (tmpl);
  ASSERT_GE (fd, 0);
  close (fd);
  const std::string path = tmpl;

  {
    sqlite3 *h = nullptr;
    ASSERT_EQ (sqlite3_open (path.c_str (), &h), SQLITE_OK);
    ASSERT_EQ (sqlite3_exec (h,
                             "CREATE TABLE tasks (id TEXT PRIMARY KEY, job_id "
                             "TEXT NOT NULL, status TEXT, result TEXT);"
                             "INSERT INTO tasks VALUES ('step-0','old-job',"
                             "'done','{\"x\":1}');",
                             nullptr, nullptr, nullptr),
               SQLITE_OK);
    sqlite3_close (h);
  }

  {
    agentos::Database db (path);
    ASSERT_TRUE (db.open ());
    EXPECT_EQ (db.load_step_result ("old-job", "step-0"), R"({"x":1})");

    // The same label in another job is now a distinct row.
    agentos::PipelinePlanStep step;
    step.id = "step-0";
    step.command = "x.y";
    step.description = "d";
    db.store_pipeline_task (agentos::TaskId ("new-job"), step, 0);
    db.update_step_result ("new-job", "step-0", R"({"x":2})");
    EXPECT_EQ (db.load_step_result ("old-job", "step-0"), R"({"x":1})");
    EXPECT_EQ (db.load_step_result ("new-job", "step-0"), R"({"x":2})");
  }
  std::remove (path.c_str ());
}
