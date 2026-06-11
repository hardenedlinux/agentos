#pragma once
/**
 * agentos/dispatcher.h
 *
 * Dispatcher — pure stateless utility class (ADR-022).
 *
 * Responsibilities:
 *   - fork/exec Worker binaries with sandbox stack applied
 *   - Pass task JSON to Worker via stdin
 *   - Redirect stdout/stderr to log file (ADR-016)
 *   - Return {run_id, pid} to Orchestrator for DB recording
 *   - Collect result.json after Orchestrator is notified of Worker exit
 *
 * Dispatcher owns no threads, no queues, no sockets.
 * All state lives in the Database (worker_runs table, ADR-016).
 * Worker exit detection is handled by the PeriodicExecutor reaper task.
 *
 * Path conventions (ADR-016):
 *   job_dir  = ~/.agentos/layers/runs/<run-id>/
 *   log_path = ~/.agentos/logs/runs/<run-id>/output.log
 * Both are derived internally from run_id; callers need not supply them.
 */

#include "agentos/home_init.h"
#include "agentos/types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agentos
{

  // ---------------------------------------------------------------------------
  // DispatchRequest — what Orchestrator hands to Dispatcher
  // ---------------------------------------------------------------------------
  struct DispatchRequest
  {
    std::string run_id;    // pre-generated UUID (Orchestrator owns generation)
    std::string step_id;   // pipeline step this Worker is executing
    std::string worker_id; // identifies the worker binary in Registry
    std::string
      binary_path;         // absolute path to worker executable (.py/.scm/ELF)
    std::string task_json; // full task payload, written to Worker stdin

    // Capability declaration from worker manifest (ADR-015)
    std::vector<std::string> fs_read;
    std::vector<std::string> fs_write;
    std::vector<int> tcp_connect_ports;
    bool network = false;
  };

  // ---------------------------------------------------------------------------
  // DispatchResult — returned synchronously to Orchestrator after fork
  // ---------------------------------------------------------------------------
  struct DispatchResult
  {
    bool ok = false;
    int pid = -1;         // child PID; -1 on failure
    std::string job_dir;  // ~/.agentos/layers/runs/<run-id>/
    std::string log_path; // ~/.agentos/logs/runs/<run-id>/output.log
    std::string error;
  };

  // ---------------------------------------------------------------------------
  // CollectResult — returned after Worker exit, result file read
  // ---------------------------------------------------------------------------
  struct CollectResult
  {
    bool ok = false;
    int exit_code = -1;
    std::string result_json;
    std::string error;
  };

  // ---------------------------------------------------------------------------
  // Dispatcher
  // ---------------------------------------------------------------------------
  class Dispatcher
  {
  public:
    Dispatcher () = default;
    ~Dispatcher () = default;

    Dispatcher (const Dispatcher &) = delete;
    Dispatcher &operator= (const Dispatcher &) = delete;

    // Fork and exec the Worker binary described by req.
    // Derives job_dir and log_path from req.run_id internally.
    // Applies sandbox stack before exec (ADR-015, ADR-016).
    // Writes req.task_json to Worker stdin.
    // Returns immediately after fork — does NOT wait for Worker to exit.
    [[nodiscard]]
    DispatchResult fork_exec (const DispatchRequest &req);

    // Read the result file written by the Worker.
    // Called by Orchestrator after receiving WorkerExited notification.
    // job_dir is taken from the DispatchResult returned by fork_exec.
    [[nodiscard]]
    CollectResult collect (const std::string &run_id,
                           const std::string &job_dir, int exit_code);

  private:
    // Derive the run layer directory from run_id (ADR-016).
    static std::filesystem::path run_dir (const std::string &run_id);

    // Derive the log file path from run_id (ADR-016).
    static std::filesystem::path log_file (const std::string &run_id);

    // Select the interpreter argv for a given binary_path.
    // Returns {"/usr/bin/env", "python3", binary_path} for .py, etc.
    static std::vector<std::string> build_argv (const std::string &binary_path);

    // Apply sandbox stack in child process after fork, before exec.
    // Returns false if any sandbox step fails (child must then _exit).
    bool apply_child_sandbox (const DispatchRequest &req,
                              const std::string &job_dir);
  };

} // namespace agentos
