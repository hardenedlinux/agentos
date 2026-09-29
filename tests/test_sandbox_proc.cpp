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

// ADR-015 GPU grant: /proc is readable inside the Worker sandbox so that a
// CUDA program started as a *child* of the Worker can read its own
// /proc/self, while Landlock's ptrace scoping keeps every process outside
// the Worker's domain (the daemon included) opaque.

#include "agentos/home_init.h"
#include "agentos/sandbox.h"

#include <gtest/gtest.h>

#include <sys/capability.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace
{
  int try_read (const std::string &path)
  {
    int fd = ::open (path.c_str (), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      return errno;
    char buf[64];
    const ssize_t n = ::read (fd, buf, sizeof buf);
    const int err = n < 0 ? errno : 0;
    ::close (fd);
    return err;
  }

  int wait_exit (pid_t pid)
  {
    int st = 0;
    ::waitpid (pid, &st, 0);
    return WIFEXITED (st) ? WEXITSTATUS (st) : -1;
  }
} // namespace

class SandboxProcTest : public ::testing::Test
{
protected:
  fs::path home_;
  void SetUp () override
  {
    char tmpl[] = "/tmp/agentos_sbproc_XXXXXX";
    home_ = ::mkdtemp (tmpl);
    ::setenv ("AGENTOS_HOME", home_.c_str (), 1);
    agentos::initialise_home (home_);
  }
  void TearDown () override
  {
    ::unsetenv ("AGENTOS_HOME");
    std::error_code ec;
    fs::remove_all (home_, ec);
  }
};

// Exit codes from the sandboxed child: bit 0 = could read the outside
// process's environ (must not), bit 1 = could not read own /proc/self/maps,
// bit 2 = its child could not read its own /proc/self/{maps,status};
// 77 = sandbox unavailable.
TEST_F (SandboxProcTest, ProcReadableButOtherProcessesOpaque)
{
  const pid_t outside = ::getpid ();
  pid_t pid = ::fork ();
  if (pid == 0)
  {
    if (cap_t caps = cap_get_proc ())
    {
      cap_value_t v = CAP_SYS_ADMIN;
      cap_set_flag (caps, CAP_EFFECTIVE, 1, &v, CAP_CLEAR);
      cap_set_proc (caps);
      cap_free (caps);
    }
    const fs::path job = home_ / "jobs" / "j";
    const fs::path run = home_ / "layers" / "runs" / "r";
    fs::create_directories (job);
    fs::create_directories (run);
    if (!agentos::apply_worker_sandbox (job.string (), run.string (), "w",
                                        {"/proc"}, {}, {}, false, false, "r"))
      ::_exit (77);

    int bits = 0;
    if (try_read ("/proc/" + std::to_string (outside) + "/environ") == 0)
      bits |= 1;
    if (try_read ("/proc/self/maps") != 0)
      bits |= 2;
    pid_t gc = ::fork ();
    if (gc == 0)
      ::_exit (try_read ("/proc/self/maps") == 0
                   && try_read ("/proc/self/status") == 0
                 ? 0
                 : 1);
    if (wait_exit (gc) != 0)
      bits |= 4;
    ::_exit (bits);
  }

  const int rc = wait_exit (pid);
  if (rc == 77)
    GTEST_SKIP () << "worker sandbox (Landlock) unavailable here";
  EXPECT_EQ (rc & 1, 0) << "a process outside the Worker's domain must "
                           "stay opaque (environ)";
  EXPECT_EQ (rc & 2, 0) << "Worker must read its own /proc/self";
  EXPECT_EQ (rc & 4, 0) << "a child of the Worker must read its own "
                           "/proc/self";
}

// A sandboxed Worker must be able to sleep (glibc/Python sleep go through
// clock_nanosleep) and have an interrupted blocking call restarted by the
// kernel (restart_syscall). Either missing is a SIGSYS kill.
TEST_F (SandboxProcTest, WorkerCanSleepAndResumeAfterSignal)
{
  pid_t pid = ::fork ();
  if (pid == 0)
  {
    if (cap_t caps = cap_get_proc ())
    {
      cap_value_t v = CAP_SYS_ADMIN;
      cap_set_flag (caps, CAP_EFFECTIVE, 1, &v, CAP_CLEAR);
      cap_set_proc (caps);
      cap_free (caps);
    }
    const fs::path job = home_ / "jobs" / "j";
    const fs::path run = home_ / "layers" / "runs" / "r";
    fs::create_directories (job);
    fs::create_directories (run);
    if (!agentos::apply_worker_sandbox (job.string (), run.string (), "w",
                                        {}, {}, {}, false, false, "r"))
      ::_exit (77);
    struct timespec ts = {0, 20 * 1000 * 1000};
    ::clock_nanosleep (CLOCK_MONOTONIC, 0, &ts, nullptr);
    // SIGSTOP/SIGCONT from the parent interrupts this sleep; the kernel
    // resumes it via restart_syscall.
    struct timespec longer = {0, 300 * 1000 * 1000};
    ::clock_nanosleep (CLOCK_MONOTONIC, 0, &longer, nullptr);
    ::_exit (0);
  }
  ::usleep (100 * 1000);
  ::kill (pid, SIGSTOP);
  ::usleep (20 * 1000);
  ::kill (pid, SIGCONT);
  int st = 0;
  ::waitpid (pid, &st, 0);
  if (WIFEXITED (st) && WEXITSTATUS (st) == 77)
    GTEST_SKIP () << "worker sandbox (Landlock) unavailable here";
  EXPECT_TRUE (WIFEXITED (st) && WEXITSTATUS (st) == 0)
    << (WIFSIGNALED (st) ? "killed by signal " + std::to_string (WTERMSIG (st))
                         : "exit " + std::to_string (WEXITSTATUS (st)));
}
