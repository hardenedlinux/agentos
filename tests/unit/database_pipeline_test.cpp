/**
 * Unit tests for ADR-022 pipeline step persistence (Database layer).
 * Covers:
 *   - store_pipeline_task
 *   - load_step_result
 *   - Schema migration for new task columns
 */

#include <gtest/gtest.h>

#include "agentos/database/database.h"
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
    // Create a unique temporary file name in the system temp directory.
    // On Linux /tmp is a suitable location; we rely on std::tmpnam.
    std::string tmpl = (fs::temp_directory_path () / "agentos_test_XXXXXX")
                         .string ();
    // After std::tmpnam the string is set.
    char buftmp[L_tmpnam];
    std::tmpnam (buftmp);
    // tmpnam may yield a filename in current directory; we prefer a fixed
    // prefix inside temp dir.
    // Simpler: use a deterministic file name inside /tmp that is unlikely to
    // collide during the unit test run.
    db_path_ = (fs::temp_directory_path () / "agentos_pipeline_test.db")
                 .string ();

    // Remove any leftover from previous runs.
    std::remove (db_path_.c_str ());

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

  EXPECT_EQ (std::string (
               reinterpret_cast<const char *> (sqlite3_column_text (stmt, 0))),
             job_id.value ());
  EXPECT_EQ (std::string (
               reinterpret_cast<const char *> (sqlite3_column_text (stmt, 1))),
             ""); // agent_id remains empty
  EXPECT_EQ (std::string (
               reinterpret_cast<const char *> (sqlite3_column_text (stmt, 2))),
             step.command);
  // params is a JSON object; we only check that the string is not empty.
  EXPECT_GT (sqlite3_column_bytes (stmt, 3), 0);
  EXPECT_EQ (std::string (
               reinterpret_cast<const char *> (sqlite3_column_text (stmt, 4))),
             step.description);
  EXPECT_EQ (sqlite3_column_int (stmt, 5), order);
  EXPECT_EQ (std::string (
               reinterpret_cast<const char *> (sqlite3_column_text (stmt, 6))),
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
                                   "UPDATE tasks SET result = ? WHERE id = ?",
                                   -1, &stmt, nullptr),
               SQLITE_OK);
    sqlite3_bind_text (stmt, 1, expected_result.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, step.id.c_str (), -1, SQLITE_TRANSIENT);
    ASSERT_EQ (sqlite3_step (stmt), SQLITE_DONE);
    sqlite3_finalize (stmt);
  }

  const std::string loaded = db_->load_step_result (step.id);
  EXPECT_EQ (loaded, expected_result);
}

// -------------------------------------------------------------------------
// load_step_result – missing step returns empty string
// -------------------------------------------------------------------------
TEST_F (DatabasePipelineTest, LoadStepResult_MissingStepReturnsEmpty)
{
  EXPECT_EQ (db_->load_step_result ("nonexistent"), "");
}

// -------------------------------------------------------------------------
// Schema migration – tasks table has the new columns
// -------------------------------------------------------------------------
TEST_F (DatabasePipelineTest, TasksTableHasNewColumns)
{
  sqlite3 *h = db_->db_handle ();
  ASSERT_NE (h, nullptr);

  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (h, "PRAGMA table_info('tasks')", -1, &stmt,
                                 nullptr),
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

  EXPECT_EQ (std::string (
               reinterpret_cast<const char *> (sqlite3_column_text (stmt, 0))),
             step2.description);
  EXPECT_EQ (sqlite3_column_int (stmt, 1), 42);
  EXPECT_EQ (std::string (
               reinterpret_cast<const char *> (sqlite3_column_text (stmt, 2))),
             step2.command);
  sqlite3_finalize (stmt);
}
