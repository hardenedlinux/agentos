#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace agentos {

class Database; // forward declaration for gc_run_layers

// ADR-015: Daemon self‑confinement.
// After initialise_home() completes, call this to lock the daemon process
// to the given home directory and TCP connect port 443.
void confine_daemon(const std::filesystem::path& home);

// ADR-016: Worker filesystem preparation.
// Must be called inside apply_worker_sandbox before the remaining sandbox
// steps (Landlock/seccomp/capability drop).
//
// `privileged` selects the strategy:
//
//   true  - privileged (enterprise): native kernel overlayfs (lowerdir=/)
//           + pivot_root + bind-mount the worker's persistent directory to
//           /home/agentos. Requires real CAP_SYS_ADMIN in the current user
//           namespace (granted via `setcap cap_sys_admin+ep` on the agentos
//           binary at install time). Capability drop later in
//           apply_worker_sandbox removes CAP_SYS_ADMIN before exec.
//
//   false - --unsafe / community (default): no filesystem namespace at
//           all. The worker sees the real host filesystem; isolation is
//           "directory-level" via Landlock path rules applied later in
//           apply_worker_sandbox. Only ensures the worker's persistent
//           directory and the run's log/layer directories exist. Requires
//           no special privileges; this is the path exercised by unit
//           tests.
//
// In both cases, sets the AGENTOS_WORKER_HOME environment variable to the
// worker's persistent storage location (/home/agentos in privileged mode,
// the real path ~/.agentos/workers/<worker-id>/ in --unsafe mode). Worker
// implementations must read this variable rather than hardcoding
// /home/agentos, so the same worker code runs unmodified under either
// strategy (Worker Contract, ADR-016).
//
// Returns true on success.
bool apply_worker_filesystem(const std::string& worker_id,
                             const std::string& run_id,
                             bool privileged);

// Detect whether the calling process holds CAP_SYS_ADMIN in its effective
// capability set. Used by apply_worker_sandbox to select between the
// privileged (native overlayfs) and --unsafe (directory-level via Landlock)
// strategies. Enterprise deployments grant this via a one-time
// `setcap cap_sys_admin+ep` on the agentos binary at install time.
bool has_cap_sys_admin();

// Make the current mount namespace's root mount propagation private
// (MS_REC|MS_PRIVATE). Required before any overlay mount / pivot_root in
// the privileged path. Returns true on success.
bool make_mount_namespace_private();

// ADR-016: Garbage collect run layers whose status is not 'running'.
// Scans the database for worker_runs where status != 'running' and removes
// the corresponding layer directory.
void gc_run_layers(Database& db);

// ADR-015/016: Worker sandbox, two-tier model selected by has_cap_sys_admin():
//
// Privileged (CAP_SYS_ADMIN present, enterprise):
//   1. unshare(CLONE_NEWNS) + make_mount_namespace_private()
//   2. apply_worker_filesystem(..., privileged=true): native overlayfs +
//      pivot_root + bind /home/agentos, sets AGENTOS_WORKER_HOME
//   3. unshare(CLONE_NEWNET) if network == false
//   4. cgroup join
//   5. Landlock (implicit job_dir rw + skills ro, plus explicit
//      fs_read/fs_write/tcp_connect_ports from the manifest)
//   6. seccomp
//   7. libcap drop -- removes CAP_SYS_ADMIN etc. before exec; the worker
//      process itself runs with no special privileges
//
// --unsafe / community (default, no CAP_SYS_ADMIN):
//   1. apply_worker_filesystem(..., privileged=false): ensures worker
//      persistent/log/layer directories exist, sets AGENTOS_WORKER_HOME to
//      the real path. No filesystem or network namespace.
//   2. cgroup join
//   3. Landlock -- same rules as above. With network == false and no
//      tcp_connect_ports, Landlock's always-on network access control
//      denies all TCP bind/connect (no CLONE_NEWNET needed).
//   4. seccomp
//   5. libcap drop (defense-in-depth; a normal unprivileged process
//      typically holds no capabilities anyway)
//
// Called in child process after fork, before exec.
// Returns true on success; false means child must _exit().
bool apply_worker_sandbox(const std::string& job_dir,
                          const std::string& worker_id,
                          const std::vector<std::string>& fs_read,
                          const std::vector<std::string>& fs_write,
                          const std::vector<int>& tcp_connect_ports,
                          bool network,
                          const std::string& run_id);

} // namespace agentos
