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
#pragma once
/**
 * worker_cgroup.h — per-Worker cgroup v2 resource limits (ADR-006 Layer 0,
 * ADR-011, ADR-015 step 1; delegation model in the ADR-015 amendment
 * "Worker resource limits").
 *
 * The daemon runs unprivileged, so it can only manage cgroups inside a
 * subtree that systemd has delegated to it (Delegate=yes). At startup it
 * moves itself into <own>/daemon (cgroup v2 forbids processes in a cgroup
 * that distributes controllers to children), enables the controllers it
 * needs, and creates <own>/workers/. Each Worker run gets
 * <own>/workers/<run_id> with memory.max / pids.max / cpu.weight written
 * before the Worker is forked; the forked child joins it before exec, so
 * the Worker and everything it spawns are accounted and limited together.
 * When the Worker is reaped, cgroup.kill removes anything it left running
 * (a Worker's descendants never outlive it) and the cgroup is removed.
 *
 * If the daemon's cgroup is not delegated (e.g. started from a login
 * shell), limits are disabled with one explicit warning at startup naming
 * the fix; Workers still run, with every other sandbox layer in force.
 */

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace agentos
{
  struct WorkerLimits
  {
    // 0 = no limit for that resource.
    std::uint64_t memory_max_bytes = 0;
    std::uint32_t pids_max = 0;
    // cgroup v2 cpu.weight, 1..10000 (100 = default share). 0 = leave unset.
    std::uint32_t cpu_weight = 0;
  };

  class WorkerCgroups
  {
  public:
    // Set up the delegated hierarchy. `cgroupfs_root` and `self_cgroup`
    // exist for tests; in production they are /sys/fs/cgroup and the
    // unified ("0::") entry of /proc/self/cgroup. Returns enabled().
    bool init (const std::filesystem::path &cgroupfs_root = "/sys/fs/cgroup",
               const std::string &self_cgroup = {});

    bool enabled () const { return enabled_; }
    const std::string &disabled_reason () const { return disabled_reason_; }

    // Create the run's cgroup with `limits` applied. Returns its path, or
    // an empty string if limits are disabled or creation failed (logged).
    std::string create (const std::string &run_id, const WorkerLimits &limits);

    // Kill whatever is still running in the run's cgroup and remove it.
    void destroy (const std::string &cgroup_path);

    // Controllers enabled for Worker cgroups (e.g. "memory pids cpu").
    const std::string &controllers () const { return controllers_; }

  private:
    bool enabled_ = false;
    std::string disabled_reason_ = "not initialised";
    std::filesystem::path workers_;
    std::string controllers_;
    std::mutex mu_;
  };

  // Write the calling process's pid into <cgroup_path>/cgroup.procs.
  // Async-signal-safe enough for use between fork and exec (open/write).
  bool join_worker_cgroup (const std::string &cgroup_path);
} // namespace agentos
