#pragma once
#include <string>
#include <vector>

namespace agentos {

// Apply the full Tier-1 sandbox stack after fork, before exec.
// This function is called in the child process.
// It sets up cgroup, seccomp, namespaces, Landlock, and drops capabilities.
// Returns true on success, false on failure (child should exit).
bool apply_sandbox(const std::string& job_dir,
                   const std::vector<std::string>& allowed_read_paths,
                   const std::vector<std::string>& allowed_write_paths,
                   const std::vector<int>& allowed_tcp_ports);

} // namespace agentos
#pragma once
#include <string>
#include <vector>

namespace agentos {

// Apply the full Tier-1 sandbox stack after fork, before exec.
// This function is called in the child process.
// It sets up cgroup, seccomp, namespaces, Landlock, and drops capabilities.
// Returns true on success, false on failure (child should exit).
bool apply_sandbox(const std::string& job_dir,
                   const std::vector<std::string>& allowed_read_paths,
                   const std::vector<std::string>& allowed_write_paths,
                   const std::vector<int>& allowed_tcp_ports);

} // namespace agentos
#pragma once
#include <string>
#include <vector>

namespace agentos {

// Apply the full Tier-1 sandbox stack after fork, before exec.
// This function is called in the child process.
// It sets up cgroup, seccomp, namespaces, Landlock, and drops capabilities.
// Returns true on success, false on failure (child should exit).
bool apply_sandbox(const std::string& job_dir,
                   const std::vector<std::string>& allowed_read_paths,
                   const std::vector<std::string>& allowed_write_paths,
                   const std::vector<int>& allowed_tcp_ports);

} // namespace agentos
