#include "agentos/dispatcher.h"
#include "agentos/home_init.h"
#include "agentos/sandbox.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace agentos
{

// ---------------------------------------------------------------------------
// Path helpers (ADR-016)
// ---------------------------------------------------------------------------

fs::path Dispatcher::run_dir (const std::string &run_id)
{
  return agentos_home () / "layers" / "runs" / run_id;
}

fs::path Dispatcher::log_file (const std::string &run_id)
{
  return agentos_home () / "logs" / "runs" / run_id / "output.log";
}

// ---------------------------------------------------------------------------
// Interpreter selection
// ---------------------------------------------------------------------------

std::vector<std::string>
Dispatcher::build_argv (const std::string &binary_path)
{
  const fs::path p (binary_path);
  const std::string ext = p.extension ().string ();

  if (ext == ".py")
    return {"/usr/bin/env", "python3", binary_path};
  if (ext == ".scm")
    return {"/usr/bin/env", "guile", binary_path};
  // Native ELF binary — exec directly.
  return {binary_path};
}

void Dispatcher::set_reap_callback (ReapCallback cb)
{
  std::lock_guard<std::mutex> lk (mutex_);
  reap_cb_ = std::move (cb);
}

// ---------------------------------------------------------------------------
// reap — called by PeriodicExecutor every 5 seconds
// ---------------------------------------------------------------------------

void Dispatcher::reap ()
{
  while (true)
    {
      int   status = 0;
      pid_t pid    = waitpid (-1, &status, WNOHANG);

      if (pid <= 0)
        break; // no more exited children

      const int exit_code = WIFEXITED (status) ? WEXITSTATUS (status) : -1;

      InFlight entry;
      ReapCallback cb;
      {
        std::lock_guard<std::mutex> lk (mutex_);
        auto it = in_flight_.find (static_cast<int> (pid));
        if (it == in_flight_.end ())
          {
            // Not our child (or already reaped) — ignore.
            continue;
          }
        entry = it->second;
        in_flight_.erase (it);
        cb = reap_cb_;
      }

      spdlog::info ("[dispatcher] reaped pid={} run_id={} exit_code={}",
                    pid, entry.run_id, exit_code);

      if (cb)
        cb (WorkerExited{entry.run_id, entry.step_id, exit_code,
                         entry.job_dir});
    }
}

// ---------------------------------------------------------------------------
// fork_exec
// ---------------------------------------------------------------------------

DispatchResult Dispatcher::fork_exec (const DispatchRequest &req)
{
  const fs::path job_dir  = run_dir (req.run_id);
  const fs::path log_path = log_file (req.run_id);

  // Ensure log directory exists.
  {
    std::error_code ec;
    fs::create_directories (log_path.parent_path (), ec);
    if (ec)
      {
        const std::string err = "cannot create log dir: " + ec.message ();
        spdlog::error ("[dispatcher] {}", err);
        return {false, -1, {}, {}, err};
      }
  }

  // Ensure run layer directory exists.
  {
    std::error_code ec;
    fs::create_directories (job_dir, ec);
    if (ec)
      {
        const std::string err = "cannot create run dir: " + ec.message ();
        spdlog::error ("[dispatcher] {}", err);
        return {false, -1, {}, {}, err};
      }
  }

  // Validate binary path exists before fork.
  if (!fs::exists (req.binary_path))
    {
      const std::string err = "binary not found: " + req.binary_path;
      spdlog::error ("[dispatcher] {}", err);
      return {false, -1, {}, {}, err};
    }

  // Open log file before fork — O_CLOEXEC so child dup2 controls it.
  int log_fd = open (log_path.c_str (),
                     O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (log_fd < 0)
    {
      const std::string err =
        std::string ("open log failed: ") + strerror (errno);
      spdlog::error ("[dispatcher] {}", err);
      return {false, -1, {}, {}, err};
    }

  // Pipe for task_json → Worker stdin.
  int pipe_fds[2];
  if (pipe2 (pipe_fds, O_CLOEXEC) != 0)
    {
      const std::string err =
        std::string ("pipe2 failed: ") + strerror (errno);
      spdlog::error ("[dispatcher] {}", err);
      close (log_fd);
      return {false, -1, {}, {}, err};
    }

  const pid_t pid = fork ();
  if (pid < 0)
    {
      const std::string err =
        std::string ("fork failed: ") + strerror (errno);
      spdlog::error ("[dispatcher] {}", err);
      close (log_fd);
      close (pipe_fds[0]);
      close (pipe_fds[1]);
      return {false, -1, {}, {}, err};
    }

  if (pid == 0)
    {
      // ── Child process ───────────────────────────────────────────────────

      // stdin ← pipe read end
      if (dup2 (pipe_fds[0], STDIN_FILENO) < 0)
        _exit (1);

      // stdout + stderr → log file
      if (dup2 (log_fd, STDOUT_FILENO) < 0)
        _exit (1);
      if (dup2 (log_fd, STDERR_FILENO) < 0)
        _exit (1);

      // O_CLOEXEC handles cleanup of log_fd and pipe_fds on exec,
      // but close explicitly now since dup2 targets stay open.
      close (log_fd);
      close (pipe_fds[0]);
      close (pipe_fds[1]);

      // Apply sandbox stack — must succeed before exec.
      if (!apply_child_sandbox (req, job_dir.string ()))
        _exit (2);

      // Build argv and exec.
      auto argv_strs = build_argv (req.binary_path);
      std::vector<const char *> argv;
      argv.reserve (argv_strs.size () + 1);
      for (const auto &s : argv_strs)
        argv.push_back (s.c_str ());
      argv.push_back (nullptr);

      execvp (argv[0], const_cast<char *const *> (argv.data ()));

      // execvp only returns on failure.
      // stderr is already redirected to log, so write there.
      const char *msg = "[dispatcher:child] execvp failed\n";
      (void)write (STDERR_FILENO, msg, strlen (msg));
      _exit (3);
    }

  // ── Parent process ────────────────────────────────────────────────────────

  close (log_fd);
  close (pipe_fds[0]); // child owns read end

  // Write task_json; child reads from stdin until EOF.
  {
    const char *buf  = req.task_json.c_str ();
    ssize_t     left = static_cast<ssize_t> (req.task_json.size ());
    while (left > 0)
      {
        ssize_t n = write (pipe_fds[1], buf, static_cast<size_t> (left));
        if (n < 0)
          {
            if (errno == EINTR)
              continue;
            spdlog::warn ("[dispatcher] write to child stdin failed: {}",
                          strerror (errno));
            break;
          }
        buf  += n;
        left -= n;
      }
    close (pipe_fds[1]); // EOF
  }

  spdlog::info ("[dispatcher] forked run_id={} pid={} step_id={} binary={}",
                req.run_id, pid, req.step_id, req.binary_path);

  // Record in-flight entry — protected by mutex (reap() runs on another thread).
  {
    std::lock_guard<std::mutex> lk (mutex_);
    in_flight_[pid] = InFlight{req.run_id, req.step_id, job_dir.string ()};
  }

  return {true, pid, job_dir.string (), log_path.string (), {}};
}

// ---------------------------------------------------------------------------
// collect
// ---------------------------------------------------------------------------

CollectResult Dispatcher::collect (const std::string &run_id,
                                   const std::string &job_dir,
                                   int                exit_code)
{
  const fs::path result_path = fs::path (job_dir) / "result.json";

  if (!fs::exists (result_path))
    {
      const std::string err =
        "result.json not found: " + result_path.string ();
      spdlog::warn ("[dispatcher] run_id={} {}", run_id, err);
      return {false, exit_code, {}, err};
    }

  std::ifstream f (result_path);
  if (!f)
    {
      const std::string err =
        "cannot open result.json: " + result_path.string ();
      spdlog::error ("[dispatcher] run_id={} {}", run_id, err);
      return {false, exit_code, {}, err};
    }

  std::ostringstream ss;
  ss << f.rdbuf ();

  spdlog::info ("[dispatcher] collected run_id={} exit_code={}", run_id,
                exit_code);
  return {true, exit_code, ss.str (), {}};
}

// ---------------------------------------------------------------------------
// apply_child_sandbox
// ---------------------------------------------------------------------------

bool Dispatcher::apply_child_sandbox (const DispatchRequest &req,
                                      const std::string     &job_dir)
{
  return apply_worker_sandbox (job_dir,
                               req.fs_read,
                               req.fs_write,
                               req.tcp_connect_ports,
                               req.network,
                               req.run_id);
}

} // namespace agentos
