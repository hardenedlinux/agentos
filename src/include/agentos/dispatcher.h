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
 *   - reap() — called by PeriodicExecutor every 5s; waitpid(-1, WNOHANG)
 *     to collect exited Workers and invoke the ReapCallback
 *
 * Dispatcher owns no threads, no event loop.
 * fork_exec() and reap() may be called from different threads;
 * the in-flight map is protected by a mutex.
 *
 * Path conventions (ADR-016):
 *   job_dir  = ~/.agentos/layers/runs/<run-id>/
 *   log_path = ~/.agentos/logs/runs/<run-id>/output.log
 * Both are derived internally from run_id; callers need not supply them.
 */

#include "agentos/home_init.h"
#include "agentos/types.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos
{

// ---------------------------------------------------------------------------
// DispatchRequest — what Orchestrator hands to Dispatcher
// ---------------------------------------------------------------------------
struct DispatchRequest
{
  std::string run_id;      // pre-generated UUID (Orchestrator owns generation)
  std::string step_id;     // pipeline step this Worker is executing
  std::string worker_id;   // identifies the worker binary in Registry
  std::string binary_path; // absolute path to worker executable (.py/.scm/ELF)
  std::string task_json;   // full task payload, written to Worker stdin

  // Capability declaration from worker manifest (ADR-015)
  std::vector<std::string> fs_read;
  std::vector<std::string> fs_write;
  std::vector<int>         tcp_connect_ports;
  bool                     network = false;
};

// ---------------------------------------------------------------------------
// DispatchResult — returned synchronously to Orchestrator after fork
// ---------------------------------------------------------------------------
struct DispatchResult
{
  bool        ok  = false;
  int         pid = -1;
  std::string job_dir;  // ~/.agentos/layers/runs/<run-id>/
  std::string log_path; // ~/.agentos/logs/runs/<run-id>/output.log
  std::string error;
};

// ---------------------------------------------------------------------------
// CollectResult — returned after Worker exit, result file read
// ---------------------------------------------------------------------------
struct CollectResult
{
  bool        ok        = false;
  int         exit_code = -1;
  std::string result_json;
  std::string error;
};

// ---------------------------------------------------------------------------
// WorkerExited — delivered to Orchestrator by reap()
// ---------------------------------------------------------------------------
struct WorkerExited
{
  std::string run_id;
  std::string step_id;
  int         exit_code = -1;
  std::string job_dir;  // needed by Orchestrator to call collect()
};

using ReapCallback = std::function<void(WorkerExited)>;

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------
class Dispatcher
{
public:
  Dispatcher () = default;
  ~Dispatcher () = default;

  Dispatcher (const Dispatcher &)            = delete;
  Dispatcher &operator= (const Dispatcher &) = delete;

  // Register the callback invoked by reap() for each exited Worker.
  // Must be called before any fork_exec() or reap() calls.
  // Thread-safe.
  void set_reap_callback (ReapCallback cb);

  // Fork and exec the Worker binary described by req.
  // Inserts {pid → run_id, step_id} into the in-flight map.
  // Returns immediately after fork.
  // Thread-safe.
  [[nodiscard]]
  DispatchResult fork_exec (const DispatchRequest &req);

  // Called by PeriodicExecutor every 5 seconds.
  // Calls waitpid(-1, WNOHANG) to collect all exited Workers.
  // For each exited pid, looks up run_id/step_id in the in-flight map,
  // removes the entry, and invokes the ReapCallback.
  // Thread-safe.
  void reap ();

  // Read the result file written by the Worker.
  // Called by Orchestrator after receiving WorkerExited via ReapCallback.
  [[nodiscard]]
  CollectResult collect (const std::string &run_id,
                         const std::string &job_dir,
                         int                exit_code);

private:
  // In-flight Worker record.
  struct InFlight
  {
    std::string run_id;
    std::string step_id;
    std::string job_dir;
  };

  // Derive the run layer directory from run_id (ADR-016).
  static std::filesystem::path run_dir (const std::string &run_id);

  // Derive the log file path from run_id (ADR-016).
  static std::filesystem::path log_file (const std::string &run_id);

  // Select the interpreter argv for a given binary_path.
  static std::vector<std::string> build_argv (const std::string &binary_path);

  // Apply sandbox stack in child process after fork, before exec.
  bool apply_child_sandbox (const DispatchRequest &req,
                            const std::string     &job_dir);

  // Protected by mutex_ — accessed from Orchestrator and PeriodicExecutor threads.
  std::unordered_map<int, InFlight> in_flight_; // pid → InFlight
  mutable std::mutex                mutex_;
  ReapCallback                      reap_cb_;
};

} // namespace agentos
