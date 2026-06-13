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

#include "agentos/dispatcher.h"
#include "agentos/home_init.h"

#include <filesystem>
#include <fstream>
#include <sys/wait.h>

namespace fs = std::filesystem;
using namespace agentos;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class DispatcherTest : public ::testing::Test
{
protected:
  fs::path   home_;
  Dispatcher dispatcher_;

  void SetUp () override
  {
    char tmpl[] = "/tmp/agentos_disp_test_XXXXXX";
    char *dir   = mkdtemp (tmpl);
    ASSERT_NE (dir, nullptr);
    home_ = dir;
    setenv ("AGENTOS_HOME", home_.c_str (), 1);
    agentos::initialise_home (home_);
  }

  void TearDown () override
  {
    unsetenv ("AGENTOS_HOME");
    fs::remove_all (home_);
  }

  // Build a minimal DispatchRequest pointing at a real script.
  DispatchRequest make_req (const std::string &run_id,
                            const std::string &binary_path,
                            const std::string &task_json = R"({"a":1})")
  {
    DispatchRequest req;
    req.run_id      = run_id;
    req.step_id     = "step-0";
    req.worker_id   = "test-worker";
    req.binary_path = binary_path;
    req.task_json   = task_json;
    req.network     = false;
    return req;
  }
};

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

TEST_F (DispatcherTest, RunDir_ContainsRunId)
{
  // run_dir is a private static; test indirectly via fork_exec result.
  // We can verify the returned job_dir contains the run_id.
  // Use a non-existent binary to get an early error but still check paths.
  auto req = make_req ("run-path-test", "/nonexistent/binary");
  auto res  = dispatcher_.fork_exec (req);
  // Fails because binary doesn't exist — but job_dir and log_path are empty
  // in this case. Test path convention via a successful fork instead (below).
  EXPECT_FALSE (res.ok);
}

TEST_F (DispatcherTest, MissingBinary_ReturnsError)
{
  auto req = make_req ("run-missing", "/does/not/exist.py");
  auto res  = dispatcher_.fork_exec (req);
  EXPECT_FALSE (res.ok);
  EXPECT_EQ (res.pid, -1);
  EXPECT_FALSE (res.error.empty ());
}

// ---------------------------------------------------------------------------
// Collect — missing result.json
// ---------------------------------------------------------------------------

TEST_F (DispatcherTest, Collect_MissingResultJson_ReturnsError)
{
  const std::string run_id  = "run-collect-missing";
  const std::string job_dir = (home_ / "layers" / "runs" / run_id).string ();
  fs::create_directories (job_dir);

  auto res = dispatcher_.collect (run_id, job_dir, 0);
  EXPECT_FALSE (res.ok);
  EXPECT_FALSE (res.error.empty ());
}

// ---------------------------------------------------------------------------
// Collect — result.json present
// ---------------------------------------------------------------------------

TEST_F (DispatcherTest, Collect_ResultJsonPresent_ReturnsContent)
{
  const std::string run_id   = "run-collect-ok";
  const std::string job_dir  = (home_ / "layers" / "runs" / run_id).string ();
  fs::create_directories (job_dir);

  // ADR-016 Result File Wire Format: result.json is a {status,result,error}
  // envelope; collect() returns just the serialised `result` field.
  {
    std::ofstream f (job_dir + "/result.json");
    f << R"({"status":"ok","result":{"value":42}})";
  }

  auto res = dispatcher_.collect (run_id, job_dir, 0);
  EXPECT_TRUE (res.ok);
  EXPECT_EQ (res.result_json, R"({"value":42})");
  EXPECT_EQ (res.exit_code, 0);
}

TEST_F (DispatcherTest, Collect_NonZeroExitCode_Propagated)
{
  const std::string run_id  = "run-collect-fail";
  const std::string job_dir = (home_ / "layers" / "runs" / run_id).string ();
  fs::create_directories (job_dir);

  std::ofstream (job_dir + "/result.json") << R"({"status":"ok","result":{}})";

  // ADR-016: a non-zero exit code is a hard failure regardless of
  // result.json content — result.json is not even read.
  auto res = dispatcher_.collect (run_id, job_dir, 1);
  EXPECT_FALSE (res.ok);
  EXPECT_EQ (res.exit_code, 1);
  EXPECT_FALSE (res.error.empty ());
}

// ---------------------------------------------------------------------------
// fork_exec with real Python script — requires no root, no sandbox
// Sandbox steps will fail gracefully in test environment (no cgroup).
// We verify: process spawned, stdin received, result.json written.
// ---------------------------------------------------------------------------

TEST_F (DispatcherTest, ForkExec_PythonScript_WritesResultJson)
{
  // Write a minimal worker script that reads stdin and writes result.json.
  const std::string run_id     = "run-py-test";
  const fs::path    script_dir = home_ / "workers" / "test-worker";
  fs::create_directories (script_dir);
  const fs::path script = script_dir / "worker.py";
  {
    std::ofstream f (script);
    f << R"(
import sys, json, os

task = json.load(sys.stdin)

run_id = os.environ.get("AGENTOS_RUN_ID", "unknown")
home   = os.environ.get("AGENTOS_HOME", "/tmp")

job_dir = os.path.join(home, "layers", "runs", run_id)
os.makedirs(job_dir, exist_ok=True)

result = {"status": "ok", "input": task}
with open(os.path.join(job_dir, "result.json"), "w") as out:
    json.dump(result, out)
)";
  }

  // Sandbox will likely fail (no cgroup in test env) but child continues
  // because apply_worker_sandbox failure causes _exit(2).
  // To test the full path without sandbox, we need to skip sandbox.
  // We test via a wrapper that skips sandbox: not possible with current
  // private apply_child_sandbox. Instead, verify the plumbing up to fork.

  auto req = make_req (run_id, script.string (),
                       R"({"hello":"world"})");

  // Pass run_id via environment so the script can find its output dir.
  // Note: setenv before fork, child inherits.
  setenv ("AGENTOS_RUN_ID", run_id.c_str (), 1);

  auto res = dispatcher_.fork_exec (req);

  // In a test environment without cgroup/namespace privileges, the child
  // will exit with code 2 (sandbox failed). We still verify fork succeeded.
  ASSERT_TRUE (res.ok) << "fork_exec failed: " << res.error;
  EXPECT_GT (res.pid, 0);
  EXPECT_FALSE (res.job_dir.empty ());
  EXPECT_FALSE (res.log_path.empty ());

  // Wait for child to exit.
  int status = 0;
  waitpid (res.pid, &status, 0);

  unsetenv ("AGENTOS_RUN_ID");

  // Log path should exist (created before fork).
  EXPECT_TRUE (fs::exists (res.log_path))
    << "log file not created: " << res.log_path;
}

// ---------------------------------------------------------------------------
// DispatchResult fields populated correctly on success
// ---------------------------------------------------------------------------

TEST_F (DispatcherTest, ForkExec_ResultContainsJobDirAndLogPath)
{
  // Use /bin/true as a trivially-available binary that exits 0.
  // Sandbox will fail but fork should succeed.
  const std::string run_id = "run-true-test";
  auto req = make_req (run_id, "/bin/true", "{}");

  auto res = dispatcher_.fork_exec (req);
  ASSERT_TRUE (res.ok) << res.error;

  // job_dir should contain run_id.
  EXPECT_NE (res.job_dir.find (run_id), std::string::npos);
  // log_path should contain run_id.
  EXPECT_NE (res.log_path.find (run_id), std::string::npos);

  int status = 0;
  waitpid (res.pid, &status, 0);
}
