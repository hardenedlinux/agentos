#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace agentos {

class Database; // forward declaration for gc_run_layers

// Apply the full Tier-1 sandbox stack after fork, before exec.
// This function is called in the child process.
// It sets up cgroup, seccomp, namespaces, Landlock, and drops capabilities.
// Returns true on success, false on failure (child should exit).
bool apply_sandbox(const std::string& job_dir,
                   const std::vector<std::string>& allowed_read_paths,
                   const std::vector<std::string>& allowed_write_paths,
                   const std::vector<int>& allowed_tcp_ports);

// ADR-015: Daemon self‑confinement.
// After initialise_home() completes, call this to lock the daemon process
// to the given home directory and TCP connect port 443.
void confine_daemon(const std::filesystem::path& home);

// ADR-016: Worker filesystem isolation (overlayfs + pivot_root).
// Must be called inside the worker's mount namespace before other sandbox steps.
// Creates the run layer directories, mounts overlayfs, pivot_root, and bind-mounts
// the worker's persistent directory to /home/agentos.
// Returns true on success.
bool apply_worker_filesystem(const std::string& worker_id,
                             const std::string& run_id);

// ADR-016: Garbage collect run layers whose status is not 'running'.
// Scans the database for worker_runs where status != 'running' and removes
// the corresponding layer directory.
void gc_run_layers(Database& db);

// ADR-015: Worker sandbox with implicit grants and network control.
// Called in child process after fork, before exec.
// Returns true on success; false means child must _exit().
// Handles: overlayfs+pivot_root, cgroup join, mount namespace,
// network namespace (if network==false), Landlock, seccomp, cap drop.
bool apply_worker_sandbox(const std::string& job_dir,
                          const std::vector<std::string>& fs_read,
                          const std::vector<std::string>& fs_write,
                          const std::vector<int>& tcp_connect_ports,
                          bool network,
                          const std::string& run_id);

} // namespace agentos
