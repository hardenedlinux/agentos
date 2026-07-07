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
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "agentos/config.h"
#include "agentos/cred_vault.h"
#include "agentos/database.h"
#include "agentos/dispatcher.h"
#include "agentos/home_init.h"
#include "agentos/llm_proxy.h"
#include "agentos/orchestrator.h"
#include "agentos/registry.h"
#include "agentos/sandbox.h"
#include "agentos/types.h"

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
// Common fixture: in-memory Database + real Registry/LlmProxy/ForgeCoordinator
// /Dispatcher/CredVault/Orchestrator (ADR-022/024/028).
// ---------------------------------------------------------------------------
class WorkerRunTest : public ::testing::Test
{
protected:
  class InMemoryDatabase : public Database
  {
  public:
    InMemoryDatabase () : Database (":memory:") {}
  };

  void SetUp () override
  {
    ASSERT_TRUE (db.open ());
    registry.init(db);

    auto r = cred_vault.start ();
    ASSERT_TRUE (r.has_value ()) << r.error ();
  }

  void TearDown () override
  {
    cred_vault.stop ();
  }

  // Declaration order = initialisation order.
  InMemoryDatabase db;
  Config config{};
  Config::Vault vault_cfg{};
  LlmProxy llm{1, 1};
  Registry registry;
  Dispatcher dispatcher{};
  forge::ForgeCoordinator forge{db, llm, registry, [] (forge::ForgeResult) {}};
  CredVault cred_vault{db, vault_cfg};
  Orchestrator orchestrator{db,
                            llm,
                            registry,
                            dispatcher,
                            forge,
                            config,
                            cred_vault,
                            [] (MasterEvent) {},
                            [] (GatewayEvent) {}};
};

// ===========================================================================
// apply_worker_filesystem
// ===========================================================================

// --unsafe / community strategy: no namespaces, no mounts, no pivot_root --
// safe to call directly in the test process. Requires no special
// privileges, exercised in normal CI.
TEST (ApplyWorkerFilesystemTest, UnsafeCreatesWorkerAndLogDirectories)
{
  TempHome home;
  const std::string worker_id = "test_worker";
  const std::string run_id = "test_run_001";

  ASSERT_TRUE (apply_worker_filesystem (worker_id, run_id, /*privileged=*/false));

  const fs::path base = home.path ();

  EXPECT_TRUE (fs::exists (base / "workers" / worker_id));
  EXPECT_TRUE (fs::exists (base / "logs" / "runs" / run_id));
  EXPECT_TRUE (fs::exists (base / "layers" / "runs" / run_id));

  const char *worker_home = std::getenv ("AGENTOS_WORKER_HOME");
  ASSERT_NE (worker_home, nullptr);
  EXPECT_EQ (fs::path (worker_home), base / "workers" / worker_id);

  unsetenv ("AGENTOS_WORKER_HOME");
}

// Privileged (enterprise) strategy: native overlayfs + pivot_root. Requires
// real CAP_SYS_ADMIN (e.g. `sudo setcap cap_sys_admin+ep` on the test
// binary). Skipped otherwise -- this is the path exercised by enterprise
// deployments, not by default CI.
TEST (ApplyWorkerFilesystemTest, PrivilegedAppliesOverlayAndPivotRoot)
{
  if (!has_cap_sys_admin ())
    GTEST_SKIP () << "Requires CAP_SYS_ADMIN "
                     "(sudo setcap cap_sys_admin+ep on the test binary)";

  TempHome home;
  const std::string worker_id = "test_worker";
  const std::string run_id = "test_run_002";

  // apply_worker_filesystem(..., privileged=true) performs pivot_root,
  // which changes the calling process's root filesystem view. Run it in a
  // forked child; the parent's filesystem view is unaffected and can check
  // the resulting directories directly.
  pid_t pid = fork ();
  ASSERT_GE (pid, 0) << "fork failed";

  if (pid == 0)
  {
    if (unshare (CLONE_NEWNS) != 0)
      _exit (10);
    if (!make_mount_namespace_private ())
      _exit (11);
    bool ok = apply_worker_filesystem (worker_id, run_id, /*privileged=*/true);
    _exit (ok ? 0 : 1);
  }

  int status = 0;
  ASSERT_EQ (waitpid (pid, &status, 0), pid);
  ASSERT_TRUE (WIFEXITED (status)) << "child did not exit normally";
  EXPECT_EQ (WEXITSTATUS (status), 0) << "apply_worker_filesystem failed";

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
  EXPECT_EQ (active[0].status, WorkerStatus::running);
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

  EXPECT_TRUE (db.get_active_worker_runs ().empty ());
}
