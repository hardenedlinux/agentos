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

// ADR-015 Worker resource limits: WorkerCgroups against a simulated
// cgroupfs (a plain directory tree standing in for /sys/fs/cgroup).

#include "agentos/worker_cgroup.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using agentos::WorkerCgroups;
using agentos::WorkerLimits;

namespace
{
  std::string slurp (const fs::path &p)
  {
    std::ifstream f (p);
    std::stringstream ss;
    ss << f.rdbuf ();
    return ss.str ();
  }
  void put (const fs::path &p, const std::string &v)
  {
    fs::create_directories (p.parent_path ());
    std::ofstream (p) << v;
  }
} // namespace

class WorkerCgroupTest : public ::testing::Test
{
protected:
  fs::path root_;
  fs::path base_;
  void SetUp () override
  {
    char tmpl[] = "/tmp/agentos_cg_XXXXXX";
    root_ = ::mkdtemp (tmpl);
    base_ = root_ / "user.slice" / "agentos.scope";
    put (base_ / "cgroup.controllers", "cpuset cpu io memory pids\n");
    put (base_ / "cgroup.subtree_control", "");
    put (base_ / "cgroup.procs", std::to_string (::getpid ()) + "\n");
  }
  void TearDown () override
  {
    std::error_code ec;
    fs::remove_all (root_, ec);
  }
};

TEST_F (WorkerCgroupTest, NotDelegated_DisablesWithReason)
{
  WorkerCgroups cg;
  EXPECT_FALSE (cg.init (root_, "/no/such/cgroup"));
  EXPECT_FALSE (cg.enabled ());
  EXPECT_NE (cg.disabled_reason ().find ("not delegated"), std::string::npos)
    << cg.disabled_reason ();
  EXPECT_EQ (cg.create ("run-1", WorkerLimits{1, 1, 1}), "");
}

TEST_F (WorkerCgroupTest, Init_MovesDaemonAndEnablesControllers)
{
  WorkerCgroups cg;
  ASSERT_TRUE (cg.init (root_, "/user.slice/agentos.scope"))
    << cg.disabled_reason ();
  EXPECT_EQ (cg.controllers (), "memory pids cpu");
  EXPECT_EQ (slurp (base_ / "daemon" / "cgroup.procs"),
             std::to_string (::getpid ()));
  EXPECT_EQ (slurp (base_ / "cgroup.subtree_control"),
             "+memory +pids +cpu");
  EXPECT_EQ (slurp (base_ / "workers" / "cgroup.subtree_control"),
             "+memory +pids +cpu");
}

TEST_F (WorkerCgroupTest, Restart_InsideDaemonCgroup_ManagesParent)
{
  WorkerCgroups first;
  ASSERT_TRUE (first.init (root_, "/user.slice/agentos.scope"));
  WorkerCgroups again;
  ASSERT_TRUE (again.init (root_, "/user.slice/agentos.scope/daemon"))
    << again.disabled_reason ();
  EXPECT_FALSE (fs::exists (base_ / "daemon" / "daemon"));
}

TEST_F (WorkerCgroupTest, Create_WritesLimits)
{
  WorkerCgroups cg;
  ASSERT_TRUE (cg.init (root_, "/user.slice/agentos.scope"));
  const std::string path
    = cg.create ("0a1b-run", WorkerLimits{1ull << 30, 100, 50});
  ASSERT_EQ (path, (base_ / "workers" / "0a1b-run").string ());
  EXPECT_EQ (slurp (fs::path (path) / "memory.max"), "1073741824");
  EXPECT_EQ (slurp (fs::path (path) / "memory.swap.max"), "0");
  EXPECT_EQ (slurp (fs::path (path) / "pids.max"), "100");
  EXPECT_EQ (slurp (fs::path (path) / "cpu.weight"), "50");

  const std::string unlimited = cg.create ("run-2", WorkerLimits{});
  ASSERT_FALSE (unlimited.empty ());
  EXPECT_FALSE (fs::exists (fs::path (unlimited) / "memory.max"))
    << "0 means no limit: nothing written";
  EXPECT_FALSE (fs::exists (fs::path (unlimited) / "pids.max"));
}

TEST_F (WorkerCgroupTest, Create_RejectsUnsafeRunId)
{
  WorkerCgroups cg;
  ASSERT_TRUE (cg.init (root_, "/user.slice/agentos.scope"));
  for (const char *bad : {"", "..", "a/b", "../escape", "x y"})
    EXPECT_EQ (cg.create (bad, WorkerLimits{1, 1, 1}), "") << bad;
}

TEST_F (WorkerCgroupTest, OnlyDelegatedControllersAreUsed)
{
  put (base_ / "cgroup.controllers", "pids\n");
  WorkerCgroups cg;
  ASSERT_TRUE (cg.init (root_, "/user.slice/agentos.scope"));
  EXPECT_EQ (cg.controllers (), "pids");
  const std::string path = cg.create ("r", WorkerLimits{1ull << 30, 10, 50});
  EXPECT_EQ (slurp (fs::path (path) / "pids.max"), "10");
  EXPECT_FALSE (fs::exists (fs::path (path) / "memory.max"));
  EXPECT_FALSE (fs::exists (fs::path (path) / "cpu.weight"));
}

TEST_F (WorkerCgroupTest, Destroy_KillsLeftovers)
{
  WorkerCgroups cg;
  ASSERT_TRUE (cg.init (root_, "/user.slice/agentos.scope"));
  const std::string path = cg.create ("r", WorkerLimits{});
  cg.destroy (path);
  // On a real cgroupfs the directory is then removed; a plain directory
  // keeps the interface files, but the kill request must have been made.
  EXPECT_EQ (slurp (fs::path (path) / "cgroup.kill"), "1");
}

TEST_F (WorkerCgroupTest, NoControllers_Disables)
{
  put (base_ / "cgroup.controllers", "cpuset io\n");
  WorkerCgroups cg;
  EXPECT_FALSE (cg.init (root_, "/user.slice/agentos.scope"));
  EXPECT_NE (cg.disabled_reason ().find ("no memory/pids/cpu"),
             std::string::npos);
}
