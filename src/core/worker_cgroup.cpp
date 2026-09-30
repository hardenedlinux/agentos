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
#include "agentos/worker_cgroup.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace agentos
{
  namespace fs = std::filesystem;

  namespace
  {
    bool write_file (const fs::path &p, const std::string &value,
                     std::string *err = nullptr)
    {
      const int fd = ::open (p.c_str (), O_WRONLY | O_CLOEXEC | O_CREAT, 0644);
      if (fd < 0)
      {
        if (err)
          *err = std::strerror (errno);
        return false;
      }
      const ssize_t n = ::write (fd, value.data (), value.size ());
      const int e = errno;
      ::close (fd);
      if (n != static_cast<ssize_t> (value.size ()))
      {
        if (err)
          *err = n < 0 ? std::strerror (e) : "short write";
        return false;
      }
      return true;
    }

    std::string read_file (const fs::path &p)
    {
      std::ifstream f (p);
      std::stringstream ss;
      ss << f.rdbuf ();
      return ss.str ();
    }

    // The unified-hierarchy ("0::<path>") entry of /proc/self/cgroup.
    std::string own_cgroup_v2 ()
    {
      std::ifstream f ("/proc/self/cgroup");
      std::string line;
      while (std::getline (f, line))
        if (line.rfind ("0::", 0) == 0)
          return line.substr (3);
      return {};
    }

    bool valid_run_id (const std::string &id)
    {
      if (id.empty () || id.size () > 128 || id == "." || id == "..")
        return false;
      for (unsigned char c : id)
        if (!(std::isalnum (c) || c == '-' || c == '_'))
          return false;
      return true;
    }
  } // namespace

  bool WorkerCgroups::init (const fs::path &cgroupfs_root,
                            const std::string &self_cgroup)
  {
    std::lock_guard<std::mutex> lk (mu_);
    enabled_ = false;

    auto disable = [&] (const std::string &why)
    {
      disabled_reason_ = why;
      spdlog::warn ("[cgroup] Worker resource limits DISABLED: {}. Workers "
                    "still run with Landlock/seccomp/capability isolation. "
                    "To enable limits, start the daemon in a delegated "
                    "cgroup, e.g.: systemd-run --user --scope -p Delegate=yes "
                    "agentos run (or a systemd user service with "
                    "Delegate=yes)",
                    why);
      return false;
    };

    const std::string rel = self_cgroup.empty () ? own_cgroup_v2 ()
                                                 : self_cgroup;
    if (rel.empty ())
      return disable ("no cgroup v2 unified hierarchy (/proc/self/cgroup has "
                      "no 0:: entry)");
    if (!fs::exists (cgroupfs_root / "cgroup.controllers")
        && self_cgroup.empty ())
      return disable ("cgroup v2 is not mounted at "
                      + cgroupfs_root.string ());

    fs::path base = cgroupfs_root / fs::path (rel).relative_path ();
    // A restart inside the same delegated scope finds itself already in
    // <base>/daemon; manage the parent in that case.
    if (base.filename () == "daemon")
      base = base.parent_path ();

    if (::access (base.c_str (), W_OK) != 0
        || ::access ((base / "cgroup.subtree_control").c_str (), W_OK) != 0)
      return disable ("the daemon's cgroup " + base.string ()
                      + " is not delegated to this user (not writable)");

    std::error_code ec;
    const fs::path daemon_cg = base / "daemon";
    fs::create_directory (daemon_cg, ec);
    if (ec)
      return disable ("cannot create " + daemon_cg.string () + ": "
                      + ec.message ());

    // cgroup v2 "no internal processes": move every process still in the
    // base cgroup (the daemon, and anything started with it in the same
    // scope) into <base>/daemon before enabling controllers for children.
    {
      std::istringstream procs (read_file (base / "cgroup.procs"));
      std::string pid;
      std::string err;
      bool moved_self = false;
      while (procs >> pid)
      {
        if (!write_file (daemon_cg / "cgroup.procs", pid, &err))
          spdlog::warn ("[cgroup] cannot move pid {} into {}: {}", pid,
                        daemon_cg.string (), err);
        if (pid == std::to_string (::getpid ()))
          moved_self = true;
      }
      if (!moved_self
          && !write_file (daemon_cg / "cgroup.procs",
                          std::to_string (::getpid ()), &err))
        return disable ("cannot move the daemon into " + daemon_cg.string ()
                        + ": " + err);
    }

    // Enable the controllers we use, among those available.
    std::set<std::string> available;
    {
      std::istringstream ss (read_file (base / "cgroup.controllers"));
      std::string c;
      while (ss >> c)
        available.insert (c);
    }
    std::string enable;
    controllers_.clear ();
    for (const char *c : {"memory", "pids", "cpu"})
      if (available.count (c))
      {
        enable += std::string (enable.empty () ? "" : " ") + "+" + c;
        controllers_ += std::string (controllers_.empty () ? "" : " ") + c;
      }
    if (enable.empty ())
      return disable ("no memory/pids/cpu controller is delegated to "
                      + base.string ());

    std::string err;
    if (!write_file (base / "cgroup.subtree_control", enable, &err))
      return disable ("cannot enable controllers in " + base.string () + ": "
                      + err);

    workers_ = base / "workers";
    fs::create_directory (workers_, ec);
    if (ec)
      return disable ("cannot create " + workers_.string () + ": "
                      + ec.message ());
    if (!write_file (workers_ / "cgroup.subtree_control", enable, &err))
      return disable ("cannot enable controllers in " + workers_.string ()
                      + ": " + err);

    // Leftovers of a previous daemon in the same delegated cgroup: nothing
    // can legitimately still be running there.
    for (const auto &e : fs::directory_iterator (workers_, ec))
      if (e.is_directory ())
      {
        write_file (e.path () / "cgroup.kill", "1");
        fs::remove (e.path (), ec);
      }

    enabled_ = true;
    disabled_reason_.clear ();
    spdlog::info ("[cgroup] Worker resource limits enabled under {} "
                  "(controllers: {})",
                  workers_.string (), controllers_);
    return true;
  }

  std::string WorkerCgroups::create (const std::string &run_id,
                                     const WorkerLimits &limits)
  {
    std::lock_guard<std::mutex> lk (mu_);
    if (!enabled_)
      return {};
    if (!valid_run_id (run_id))
    {
      spdlog::error ("[cgroup] refusing cgroup for invalid run id '{}'",
                     run_id);
      return {};
    }
    const fs::path cg = workers_ / run_id;
    std::error_code ec;
    if (!fs::create_directory (cg, ec) && ec)
    {
      spdlog::error ("[cgroup] cannot create {}: {}", cg.string (),
                     ec.message ());
      return {};
    }

    std::string err;
    auto set = [&] (const char *file, const std::string &value)
    {
      if (!write_file (cg / file, value, &err))
        spdlog::warn ("[cgroup] run {}: cannot set {}={}: {}", run_id, file,
                      value, err);
    };
    const bool has_mem = controllers_.find ("memory") != std::string::npos;
    const bool has_pids = controllers_.find ("pids") != std::string::npos;
    const bool has_cpu = controllers_.find ("cpu") != std::string::npos;
    if (has_mem && limits.memory_max_bytes > 0)
    {
      set ("memory.max", std::to_string (limits.memory_max_bytes));
      // The limit must not be escapable through swap.
      set ("memory.swap.max", "0");
    }
    if (has_pids && limits.pids_max > 0)
      set ("pids.max", std::to_string (limits.pids_max));
    if (has_cpu && limits.cpu_weight > 0)
      set ("cpu.weight", std::to_string (limits.cpu_weight));
    return cg.string ();
  }

  void WorkerCgroups::destroy (const std::string &cgroup_path)
  {
    if (cgroup_path.empty ())
      return;
    const fs::path cg (cgroup_path);
    // cgroup.kill (kernel 5.14+) SIGKILLs every process in the cgroup,
    // including descendants the Worker left behind when it exited.
    write_file (cg / "cgroup.kill", "1");
    for (int i = 0; i < 50; ++i)
    {
      if (::rmdir (cg.c_str ()) == 0 || errno == ENOENT)
        return;
      if (errno != EBUSY)
        break;
      std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    spdlog::warn ("[cgroup] cannot remove {}: {}", cg.string (),
                  std::strerror (errno));
  }

  bool join_worker_cgroup (const std::string &cgroup_path)
  {
    const std::string procs = cgroup_path + "/cgroup.procs";
    const int fd = ::open (procs.c_str (), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
      return false;
    const std::string pid = std::to_string (::getpid ());
    const bool ok = ::write (fd, pid.data (), pid.size ())
                    == static_cast<ssize_t> (pid.size ());
    ::close (fd);
    return ok;
  }
} // namespace agentos
