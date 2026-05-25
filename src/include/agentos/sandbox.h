#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace agentos {

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

// ADR-015: Worker sandbox with implicit grants and network control.
// This function applies the full sandbox stack (cgroup, mount namespace,
// network namespace if network==false, Landlock rules derived from the
// explicit fs_read/fs_write/tcp_connect_ports lists plus implicit grants
// for job_dir and skills directory, seccomp, and capability drop).
void apply_worker_sandbox(const std::string& job_dir,
                          const std::vector<std::string>& fs_read,
                          const std::vector<std::string>& fs_write,
                          const std::vector<int>& tcp_connect_ports,
                          bool network);

} // namespace agentos
