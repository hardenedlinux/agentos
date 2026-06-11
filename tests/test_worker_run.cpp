/**
 * tests/test_worker_run.cpp
 *
 * Unit tests for:
 *   - Worker filesystem isolation (ADR-016)
 *   - GC run layers (ADR-016)
 *   - Database worker_runs methods (ADR-016)
 *   - ForgeDatabase last_code_path (ADR-016/019)
 *   - Orchestrator::resume_in_flight (ADR-005)
 */
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/types.h>
#include <unistd.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "agentos/database/database.h"
#include "agentos/dispatcher.h"
#include "agentos/forge/forge_database.h"
#include "agentos/home_init.h"
#include "agentos/orchestrator.h"
#include "agentos/registry.h"
#include "agentos/sandbox.h"
#include "agentos/scheduler.h"
#include "agentos/types.h"
#include "agentos/verifier.h"

using namespace agentos;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: temporary AGENTOS_HOME directory
// ---------------------------------------------------------------------------
class TempHome
{
public:
  TempHome ()
  {
    char tmpl[] = "/tmp/agentos_test_XXXXXX";
    char *dir = mkdtemp (tmpl);
    if (!dir)
      throw std::runtime_error ("mkdtemp failed");
    path_ = dir;
    old_home_
      = std::getenv ("AGENTOS_HOME") ? std::getenv ("AGENTOS_HOME") : "";
    setenv ("AGENTOS_HOME", path_.c_str (), 1);
  }

  ~TempHome ()
  {
    if (!old_home_.empty ())
      setenv ("AGENTOS_HOME", old_home_.c_str (), 1);
    else
      unsetenv ("AGENTOS_HOME");
    std::error_code ec;
    fs::remove_all (path_, ec);
  }

  const std::string &path () const
  {
    return path_;
  }

private:
  std::string path_;
  std::string old_home_;
};

// ---------------------------------------------------------------------------
// Common fixture: in-memory Database + real Registry/Scheduler/Orchestrator
// ---------------------------------------------------------------------------
class WorkerRunTest : public ::testing::Test
{
protected:
  class InMemoryDatabase : public Database
  {
  public:
    InMemoryDatabase () : Database (":memory:") {}
  };

  // Declaration order = initialisation order.
  InMemoryDatabase db;
  Registry registry{db};
  Verifier verifier{registry};
  Dispatcher dispatcher{};
  SchedulerConfig schedConfig{};
  Scheduler scheduler{registry, dispatcher, schedConfig, db};
  Orchestrator orchestrator{registry, verifier, scheduler, dispatcher, db};
};

// ===========================================================================
// apply_worker_filesystem
// ===========================================================================
TEST (ApplyWorkerFilesystemTest, CreatesRequiredDirectories)
{
  if (geteuid () != 0)
    GTEST_SKIP () << "Requires root (mount/pivot_root)";

  TempHome home;
  const std::string worker_id = "test_worker";
  const std::string run_id = "test_run_001";

  EXPECT_TRUE (apply_worker_filesystem (worker_id, run_id));

  const fs::path base = home.path ();
  const fs::path layers_dir = base / "layers" / "runs" / run_id;

  EXPECT_TRUE (fs::exists (layers_dir / "upper"));
  EXPECT_TRUE (fs::exists (layers_dir / "work"));
  EXPECT_TRUE (fs::exists (base / "logs" / "runs" / run_id));
}

// ===========================================================================
// gc_run_layers
// ===========================================================================
TEST (GcRunLayersTest, RemovesCompletedLayerDirectory)
{
  TempHome home;

  const std::string layer_path = home.path () + "/layers/runs/test_run_gc";
  fs::create_directories (layer_path);
  ASSERT_TRUE (fs::exists (layer_path));

  Database db (":memory:");
  ASSERT_TRUE (db.open ());

  WorkerRun run;
  run.run_id = "test_run_gc";
  run.worker_id = "test_worker";
  run.status = WorkerStatus::completed;
  run.layer_path = layer_path;
  run.log_path = home.path () + "/logs/runs/test_run_gc/output.log";
  db.insert_worker_run (run);

  gc_run_layers (db);

  EXPECT_FALSE (fs::exists (layer_path));
}

TEST (GcRunLayersTest, PreservesRunningLayerDirectory)
{
  TempHome home;

  const std::string layer_path = home.path () + "/layers/runs/test_run_running";
  fs::create_directories (layer_path);
  ASSERT_TRUE (fs::exists (layer_path));

  Database db (":memory:");
  ASSERT_TRUE (db.open ());

  WorkerRun run;
  run.run_id = "test_run_running";
  run.worker_id = "test_worker";
  run.status = WorkerStatus::running;
  run.layer_path = layer_path;
  run.log_path = home.path () + "/logs/runs/test_run_running/output.log";
  db.insert_worker_run (run);

  gc_run_layers (db);

  EXPECT_TRUE (fs::exists (layer_path));
}

// ===========================================================================
// Database worker_runs
// ===========================================================================
TEST (DatabaseWorkerRunTest, InsertAndGetActiveRuns)
{
  Database db (":memory:");
  ASSERT_TRUE (db.open ());

  WorkerRun run;
  run.run_id = "run_001";
  run.worker_id = "worker_1";
  run.pid = 1234;
  run.started_at = 1000;
  run.status = WorkerStatus::running;
  run.layer_path = "/tmp/layers/runs/run_001";
  run.log_path = "/tmp/logs/runs/run_001/output.log";
  db.insert_worker_run (run);

  const auto active = db.get_active_worker_runs ();
  ASSERT_EQ (active.size (), 1u);
  EXPECT_EQ (active[0].run_id, "run_001");
  EXPECT_EQ (active[0].status, "running");
}

TEST (DatabaseWorkerRunTest, UpdateStatusToCompleted)
{
  Database db (":memory:");
  ASSERT_TRUE (db.open ());

  WorkerRun run;
  run.run_id = "run_002";
  run.worker_id = "worker_2";
  run.pid = 5678;
  run.started_at = 2000;
  run.status = WorkerStatus::running;
  run.layer_path = "/tmp/layers/runs/run_002";
  run.log_path = "/tmp/logs/runs/run_002/output.log";
  db.insert_worker_run (run);

  run.status = WorkerStatus::completed;
  run.ended_at = 3000;
  run.exit_code = 0;
  db.update_worker_run (run);

  EXPECT_TRUE (db.get_active_worker_runs ().empty ());
}

TEST (DatabaseWorkerRunTest, MarkAllRunningAsCrashed)
{
  Database db (":memory:");
  ASSERT_TRUE (db.open ());

  for (const auto &[id, pid] :
       std::initializer_list<std::pair<std::string, int>>{{"run_a", 111},
                                                          {"run_b", 222}})
  {
    WorkerRun run;
    run.run_id = id;
    run.worker_id = "w";
    run.pid = pid;
    run.started_at = 100;
    run.status = WorkerStatus::running;
    run.layer_path = "/tmp/layers/runs/" + id;
    run.log_path = "/tmp/logs/runs/" + id + "/output.log";
    db.insert_worker_run (run);
  }

  db.mark_all_running_as_crashed ();

