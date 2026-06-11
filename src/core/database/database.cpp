#include "agentos/database/database.h"
#include "agentos/forge_pipeline_job.h"
#include "agentos/home_init.h"
#include <chrono>
#include <cstring>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

namespace agentos
{

  // ---------------------------------------------------------------------------
  // Stmt — RAII wrapper for sqlite3_stmt
  // ---------------------------------------------------------------------------

  struct Stmt
  {
    sqlite3_stmt *s = nullptr;
    explicit Stmt (sqlite3_stmt *s) : s (s) {}
    ~Stmt ()
    {
      if (s)
        sqlite3_finalize (s);
    }
    Stmt (const Stmt &) = delete;
    Stmt &operator= (const Stmt &) = delete;
    operator sqlite3_stmt * ()
    {
      return s;
    }
  };

  // ---------------------------------------------------------------------------
  // Static helpers
  // ---------------------------------------------------------------------------

  static int64_t now_unix ()
  {
    return static_cast<int64_t> (
      std::chrono::duration_cast<std::chrono::seconds> (
        std::chrono::system_clock::now ().time_since_epoch ())
        .count ());
  }

  // ---------------------------------------------------------------------------
  // Database — construction / open / close
  // ---------------------------------------------------------------------------

  Database::Database (const std::string &db_path)
  {
    db_path_
      = db_path.empty () ? (agentos_home () / "agentos.db").string () : db_path;
  }

  Database::~Database ()
  {
    close ();
  }

  bool Database::open ()
  {
    if (db_)
    {
      spdlog::warn ("[database] already open");
      return true;
    }

    int rc = sqlite3_open (db_path_.c_str (), &db_);
    if (rc != SQLITE_OK)
    {
      spdlog::error ("[database] open failed: {}", sqlite3_errmsg (db_));
      sqlite3_close (db_);
      db_ = nullptr;
      return false;
    }

    sqlite3_busy_timeout (db_, 5000);

    static const char *pragmas = R"(
    PRAGMA journal_mode = WAL;
    PRAGMA synchronous  = NORMAL;
    PRAGMA foreign_keys = ON;
  )";
    if (!exec_ddl (pragmas))
      return false;

    static const char *schema = R"(
    CREATE TABLE IF NOT EXISTS jobs (
        id         TEXT PRIMARY KEY,
        phase      TEXT NOT NULL DEFAULT 'planning',
        payload    TEXT,
        plan       TEXT,
        updated_at INTEGER NOT NULL
    );
    CREATE TABLE IF NOT EXISTS tasks (
        id       TEXT PRIMARY KEY,
        job_id   TEXT NOT NULL,
        agent_id TEXT,
        method   TEXT,
        params   TEXT,
        status   TEXT DEFAULT 'pending',
        FOREIGN KEY (job_id) REFERENCES jobs(id)
    );
    CREATE TABLE IF NOT EXISTS worker_runs (
        run_id     TEXT PRIMARY KEY,
        worker_id  TEXT NOT NULL,
        pid        INTEGER NOT NULL,
        started_at INTEGER NOT NULL,
        ended_at   INTEGER,
        exit_code  INTEGER,
        status     TEXT NOT NULL DEFAULT 'running',
        layer_path TEXT NOT NULL,
        log_path   TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS forge_pipeline_jobs (
        id                    TEXT PRIMARY KEY,
        task_id               TEXT NOT NULL,
        status                TEXT NOT NULL DEFAULT 'drafting',
        requirement_json      TEXT,
        writer_output_json    TEXT,
        reviewer_verdict_json TEXT,
        feedback              TEXT,
        attempt               INTEGER DEFAULT 0,
        max_attempts          INTEGER DEFAULT 3,
        last_code_path        TEXT,
        created_at            INTEGER NOT NULL,
        updated_at            INTEGER NOT NULL
    );
    CREATE TABLE IF NOT EXISTS agents (
        id          TEXT PRIMARY KEY,
        role        TEXT NOT NULL,
        binary_path TEXT NOT NULL,
        manifest    TEXT NOT NULL,
        approved_by TEXT NOT NULL,
        approved_at INTEGER NOT NULL,
        enabled     INTEGER NOT NULL DEFAULT 1
    );
    CREATE TABLE IF NOT EXISTS capabilities (
        agent_id     TEXT NOT NULL REFERENCES agents(id),
        method       TEXT NOT NULL,
        description  TEXT NOT NULL,
        input_schema TEXT NOT NULL,
        cpu_weight   INTEGER,
        memory_mb    INTEGER,
        PRIMARY KEY (agent_id, method)
    );
    CREATE TABLE IF NOT EXISTS human_reviews (
        id          TEXT PRIMARY KEY,
        forge_id    TEXT REFERENCES forge_pipeline_jobs(id),
        reason      TEXT NOT NULL,
        artifacts   TEXT NOT NULL,
        status      TEXT DEFAULT 'pending',
        decision    TEXT,
        reviewed_at INTEGER
    );
    CREATE TABLE IF NOT EXISTS access_keys (
        id             TEXT PRIMARY KEY,
        key            TEXT NOT NULL,
        key_hash       TEXT NOT NULL,
        key_salt       TEXT NOT NULL,
        description    TEXT,
        role           TEXT NOT NULL,
        created_at     INTEGER NOT NULL,
        expires_at     INTEGER,
        last_used_at   INTEGER,
        revoked_at     INTEGER,
        revoked_reason TEXT
    );
  )";
    if (!exec_ddl (schema))
      return false;

    // ADR-022: add columns to tasks table (idempotent)
    {
      auto maybe_add_column = [this] (const char *ddl) {
        char *err = nullptr;
        sqlite3_exec (db_, ddl, nullptr, nullptr, &err);
        if (err)
        {
          spdlog::debug ("[database] migration DDL '{}': {}", ddl, err);
          sqlite3_free (err);
        }
      };
      maybe_add_column ("ALTER TABLE tasks ADD COLUMN result TEXT");
      maybe_add_column ("ALTER TABLE tasks ADD COLUMN description TEXT");
      maybe_add_column ("ALTER TABLE tasks ADD COLUMN step_order INTEGER");
      maybe_add_column ("ALTER TABLE tasks ADD COLUMN started_at INTEGER");
      maybe_add_column ("ALTER TABLE tasks ADD COLUMN completed_at INTEGER");
    }

    spdlog::info ("[database] opened {}", db_path_);
    return true;
  }

  void Database::close ()
  {
    if (db_)
    {
      sqlite3_close (db_);
      db_ = nullptr;
      spdlog::info ("[database] closed");
    }
  }

  bool Database::is_open () const
  {
    return db_ != nullptr;
  }

  sqlite3 *Database::db_handle () const
  {
    return db_;
  }

  // ---------------------------------------------------------------------------
  // Private helpers
  // ---------------------------------------------------------------------------

  bool Database::exec_ddl (const char *sql)
  {
    char *errmsg = nullptr;
    int rc = sqlite3_exec (db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK)
    {
      spdlog::error ("[database] DDL failed: {}", errmsg);
      sqlite3_free (errmsg);
      return false;
    }
    return true;
  }

  sqlite3_stmt *Database::prepare (const char *sql)
  {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2 (db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
      spdlog::error ("[database] prepare failed: {} — {}", sqlite3_errmsg (db_),
                     sql);
      return nullptr;
    }
    return stmt;
  }

  // ---------------------------------------------------------------------------
  // Column / bind helpers — file-scope, no Database membership needed
  // ---------------------------------------------------------------------------

  static void bind_optional_int64 (sqlite3_stmt *stmt, int col,
                                   const std::optional<int64_t> &val)
  {
    if (val)
      sqlite3_bind_int64 (stmt, col, *val);
    else
      sqlite3_bind_null (stmt, col);
  }

  static void bind_optional_int (sqlite3_stmt *stmt, int col,
                                 const std::optional<int> &val)
  {
    if (val)
      sqlite3_bind_int (stmt, col, *val);
    else
      sqlite3_bind_null (stmt, col);
  }

  static std::string column_text_or_empty (sqlite3_stmt *stmt, int col)
  {
    const auto *p
      = reinterpret_cast<const char *> (sqlite3_column_text (stmt, col));
    return p ? p : "";
  }

  static std::optional<int64_t> column_int64_opt (sqlite3_stmt *stmt, int col)
  {
    if (sqlite3_column_type (stmt, col) == SQLITE_NULL)
      return std::nullopt;
    return sqlite3_column_int64 (stmt, col);
  }

  static std::optional<int> column_int_opt (sqlite3_stmt *stmt, int col)
  {
    if (sqlite3_column_type (stmt, col) == SQLITE_NULL)
      return std::nullopt;
    return sqlite3_column_int (stmt, col);
  }

  // Database member forwarders — preserve the declared interface in database.h
  std::string Database::column_text_or_empty (sqlite3_stmt *stmt, int col)
  {
    return ::agentos::column_text_or_empty (stmt, col);
  }

  std::optional<int64_t> Database::column_int64_opt (sqlite3_stmt *stmt,
                                                     int col)
  {
    return ::agentos::column_int64_opt (stmt, col);
  }

  std::optional<int> Database::column_int_opt (sqlite3_stmt *stmt, int col)
  {
    return ::agentos::column_int_opt (stmt, col);
  }

  void Database::bind_optional_int64 (sqlite3_stmt *stmt, int col,
                                      const std::optional<int64_t> &val)
  {
    ::agentos::bind_optional_int64 (stmt, col, val);
  }

  void Database::bind_optional_int (sqlite3_stmt *stmt, int col,
                                    const std::optional<int> &val)
  {
    ::agentos::bind_optional_int (stmt, col, val);
  }

  template <typename Fn> bool Database::with_transaction (Fn &&fn)
  {
    if (!exec_ddl ("BEGIN"))
      return false;
    bool ok = false;
    try
    {
      ok = fn ();
    }
    catch (...)
    {
      spdlog::error ("[database] unexpected exception inside transaction");
    }
    if (!ok)
    {
      exec_ddl ("ROLLBACK");
      return false;
    }
    return exec_ddl ("COMMIT");
  }

  // ---------------------------------------------------------------------------
  // Job table
  // ---------------------------------------------------------------------------

  void Database::store_job (const Task &task)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
      INSERT OR REPLACE INTO jobs (id, phase, payload, updated_at)
      VALUES (?, ?, ?, ?)
  )"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, task.id.value ().c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, db::job_phase::planning.data (), -1,
                       SQLITE_STATIC);
    sqlite3_bind_text (stmt, 3, task.input_json.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 4, now_unix ());

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] store_job: {}", sqlite3_errmsg (db_));
  }

  void Database::update_job_phase (const TaskId &id, const std::string &phase)
  {
    if (!db_)
      return;
    Stmt stmt (
      prepare ("UPDATE jobs SET phase = ?, updated_at = ? WHERE id = ?"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, phase.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 2, now_unix ());
    sqlite3_bind_text (stmt, 3, id.value ().c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_job_phase: {}", sqlite3_errmsg (db_));
  }

  void Database::update_job_plan (const TaskId &id,
                                  const std::string &plan_json)
  {
    if (!db_)
      return;
    Stmt stmt (
      prepare ("UPDATE jobs SET plan = ?, updated_at = ? WHERE id = ?"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, plan_json.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 2, now_unix ());
    sqlite3_bind_text (stmt, 3, id.value ().c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_job_plan: {}", sqlite3_errmsg (db_));
  }

  std::string Database::load_plan_json (const TaskId &job_id)
  {
    if (!db_)
      return "";
    Stmt stmt (prepare ("SELECT plan FROM jobs WHERE id = ?"));
    if (!stmt.s)
      return "";

    sqlite3_bind_text (stmt, 1, job_id.value ().c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) == SQLITE_ROW)
      return column_text_or_empty (stmt, 0);
    return "";
  }

  std::vector<Database::InFlightJob> Database::resume_in_flight ()
  {
    std::vector<InFlightJob> jobs;
    if (!db_)
      return jobs;

    Stmt stmt (prepare (
      "SELECT id, plan FROM jobs WHERE phase NOT IN ('done','failed')"));
    if (!stmt.s)
      return jobs;

    while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      InFlightJob j;
      j.job_id = TaskId (column_text_or_empty (stmt, 0));
      j.plan_json = column_text_or_empty (stmt, 1);
      jobs.push_back (std::move (j));
    }
    return jobs;
  }

  // ---------------------------------------------------------------------------
  // Task table
  // ---------------------------------------------------------------------------

  void Database::store_task (const TaskId &job_id, const PlanStep &step)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
      INSERT OR REPLACE INTO tasks (id, job_id, agent_id, method, params, status)
      VALUES (?, ?, '', ?, ?, 'pending')
  )"));
    if (!stmt.s)
      return;

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    for (const auto &[k, v] : step.args)
    {
      w.Key (k.c_str ());
      w.String (v.c_str ());
    }
    w.EndObject ();

    sqlite3_bind_text (stmt, 1, step.id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, job_id.value ().c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, step.command.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4, buf.GetString (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] store_task: {}", sqlite3_errmsg (db_));
  }

  // ---------------------------------------------------------------------------
  // ADR-022 pipeline step persistence
  // ---------------------------------------------------------------------------

  void Database::store_pipeline_task (const TaskId &job_id,
                                      const PipelinePlanStep &step,
                                      int step_order)
  {
    if (!db_)
      return;

    Stmt stmt (prepare (R"(
      INSERT OR REPLACE INTO tasks
          (id, job_id, agent_id, method, params,
           description, step_order, status)
      VALUES (?, ?, '', ?, ?, ?, ?, 'pending')
    )"));
    if (!stmt.s)
      return;

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    for (const auto &[k, v] : step.params)
    {
      w.Key (k.c_str ());
      w.String (v.c_str ());
    }
    w.EndObject ();

    sqlite3_bind_text (stmt, 1, step.id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, job_id.value ().c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, step.command.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4, buf.GetString (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 5, step.description.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 6, step_order);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] store_pipeline_task: {}",
                     sqlite3_errmsg (db_));
  }

  std::string Database::load_step_result (const std::string &step_id)
  {
    if (!db_)
      return "";

    Stmt stmt (prepare ("SELECT result FROM tasks WHERE id = ?"));
    if (!stmt.s)
      return "";

    sqlite3_bind_text (stmt, 1, step_id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) == SQLITE_ROW)
      return column_text_or_empty (stmt, 0);
    return "";
  }

  // ---------------------------------------------------------------------------
  // WorkerRun table
  // types.h WorkerRun uses magic numbers (ended_at==0, exit_code==-1).
  // We map these to NULL in the database and restore them on read.
  // ---------------------------------------------------------------------------

  static WorkerRun row_to_worker_run (sqlite3_stmt *stmt)
  {
    WorkerRun run;
    run.run_id = column_text_or_empty (stmt, 0);
    run.worker_id = column_text_or_empty (stmt, 1);
    run.pid = sqlite3_column_int (stmt, 2);
    run.started_at = sqlite3_column_int64 (stmt, 3);
    run.ended_at = sqlite3_column_type (stmt, 4) == SQLITE_NULL
                     ? 0
                     : sqlite3_column_int64 (stmt, 4);
    run.exit_code = sqlite3_column_type (stmt, 5) == SQLITE_NULL
                      ? -1
                      : sqlite3_column_int (stmt, 5);
    run.status = column_text_or_empty (stmt, 6);
    run.layer_path = column_text_or_empty (stmt, 7);
    run.log_path = column_text_or_empty (stmt, 8);
    return run;
  }

  void Database::insert_worker_run (const WorkerRun &run)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
      INSERT INTO worker_runs
          (run_id, worker_id, pid, started_at, ended_at, exit_code,
           status, layer_path, log_path)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
  )"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, run.run_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, run.worker_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 3, run.pid);
    sqlite3_bind_int64 (stmt, 4, run.started_at);

    if (run.ended_at == 0)
      sqlite3_bind_null (stmt, 5);
    else
      sqlite3_bind_int64 (stmt, 5, run.ended_at);

    if (run.exit_code == -1)
      sqlite3_bind_null (stmt, 6);
    else
      sqlite3_bind_int (stmt, 6, run.exit_code);

    sqlite3_bind_text (stmt, 7, run.status.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 8, run.layer_path.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 9, run.log_path.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] insert_worker_run: {}", sqlite3_errmsg (db_));
  }

  void Database::update_worker_run (const WorkerRun &run)
  {
    if (!db_)
      return;
    Stmt stmt (prepare ("UPDATE worker_runs SET ended_at=?, exit_code=?, "
                        "status=? WHERE run_id=?"));
    if (!stmt.s)
      return;

    if (run.ended_at == 0)
      sqlite3_bind_null (stmt, 1);
    else
      sqlite3_bind_int64 (stmt, 1, run.ended_at);

    if (run.exit_code == -1)
      sqlite3_bind_null (stmt, 2);
    else
      sqlite3_bind_int (stmt, 2, run.exit_code);

    sqlite3_bind_text (stmt, 3, run.status.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4, run.run_id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_worker_run: {}", sqlite3_errmsg (db_));
  }

  std::vector<WorkerRun> Database::get_active_worker_runs ()
  {
    std::vector<WorkerRun> runs;
    if (!db_)
      return runs;

    Stmt stmt (prepare (R"(
      SELECT run_id, worker_id, pid, started_at, ended_at, exit_code,
             status, layer_path, log_path
      FROM worker_runs WHERE status = 'running'
  )"));
    if (!stmt.s)
      return runs;

    while (sqlite3_step (stmt) == SQLITE_ROW)
      runs.push_back (row_to_worker_run (stmt));
    return runs;
  }

  std::vector<WorkerRun> Database::get_all_worker_runs ()
  {
    std::vector<WorkerRun> runs;
    if (!db_)
      return runs;

    Stmt stmt (prepare (R"(
      SELECT run_id, worker_id, pid, started_at, ended_at, exit_code,
             status, layer_path, log_path
      FROM worker_runs
  )"));
    if (!stmt.s)
      return runs;

    while (sqlite3_step (stmt) == SQLITE_ROW)
      runs.push_back (row_to_worker_run (stmt));
    return runs;
  }

  void Database::mark_all_running_as_crashed ()
  {
    if (!db_)
      return;
    Stmt stmt (prepare ("UPDATE worker_runs SET status='crashed', ended_at=? "
                        "WHERE status='running'"));
    if (!stmt.s)
      return;

    sqlite3_bind_int64 (stmt, 1, now_unix ());

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] mark_all_running_as_crashed: {}",
                     sqlite3_errmsg (db_));
  }

  // ---------------------------------------------------------------------------
  // ForgePipelineJob table
  // created_at/updated_at stored as INTEGER (unix seconds);
  // ForgePipelineJob carries them as ISO-8601 strings — convert both ways.
  // ---------------------------------------------------------------------------

  static ForgePipelineJob row_to_forge_job (sqlite3_stmt *stmt)
  {
    ForgePipelineJob fj;
    fj.id = column_text_or_empty (stmt, 0);
    fj.task_id = column_text_or_empty (stmt, 1);
    fj.status = column_text_or_empty (stmt, 2);
    fj.requirement_json = column_text_or_empty (stmt, 3);
    fj.writer_output_json = column_text_or_empty (stmt, 4);
    fj.reviewer_verdict_json = column_text_or_empty (stmt, 5);
    fj.feedback = column_text_or_empty (stmt, 6);
    fj.attempt = sqlite3_column_int (stmt, 7);
    fj.max_attempts = sqlite3_column_int (stmt, 8);
    fj.last_code_path = column_text_or_empty (stmt, 9);
    fj.created_at = sqlite3_column_int64 (stmt, 10);
    fj.updated_at = sqlite3_column_int64 (stmt, 11);
    return fj;
  }

  void Database::store_forge_pipeline_job (const ForgePipelineJob &job)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
      INSERT OR REPLACE INTO forge_pipeline_jobs
          (id, task_id, status, requirement_json, writer_output_json,
           reviewer_verdict_json, feedback, attempt, max_attempts,
           last_code_path, created_at, updated_at)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  )"));
    if (!stmt.s)
      return;

    const int64_t ts = now_unix ();
    sqlite3_bind_text (stmt, 1, job.id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, job.task_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, job.status.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4, job.requirement_json.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 5, job.writer_output_json.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 6, job.reviewer_verdict_json.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 7, job.feedback.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 8, job.attempt);
    sqlite3_bind_int (stmt, 9, job.max_attempts);
    sqlite3_bind_text (stmt, 10, job.last_code_path.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 11, ts);
    sqlite3_bind_int64 (stmt, 12, ts);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] store_forge_pipeline_job: {}",
                     sqlite3_errmsg (db_));
  }

  void Database::update_forge_pipeline_job (const ForgePipelineJob &job)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
      UPDATE forge_pipeline_jobs SET
          status = ?, requirement_json = ?, writer_output_json = ?,
          reviewer_verdict_json = ?, feedback = ?, attempt = ?,
          max_attempts = ?, last_code_path = ?, updated_at = ?
      WHERE id = ?
  )"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, job.status.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, job.requirement_json.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, job.writer_output_json.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4, job.reviewer_verdict_json.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 5, job.feedback.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 6, job.attempt);
    sqlite3_bind_int (stmt, 7, job.max_attempts);
    sqlite3_bind_text (stmt, 8, job.last_code_path.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 9, now_unix ());
    sqlite3_bind_text (stmt, 10, job.id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_forge_pipeline_job: {}",
                     sqlite3_errmsg (db_));
  }

  void Database::update_forge_pipeline_job_status (const std::string &forge_id,
                                                   const std::string &status)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (
      "UPDATE forge_pipeline_jobs SET status=?, updated_at=? WHERE id=?"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, status.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 2, now_unix ());
    sqlite3_bind_text (stmt, 3, forge_id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_forge_pipeline_job_status: {}",
                     sqlite3_errmsg (db_));
  }

  std::optional<ForgePipelineJob>
  Database::load_forge_pipeline_job (const std::string &forge_id)
  {
    if (!db_)
      return std::nullopt;
    Stmt stmt (prepare (R"(
      SELECT id, task_id, status, requirement_json,
             writer_output_json, reviewer_verdict_json,
             feedback, attempt, max_attempts,
             last_code_path, created_at, updated_at
      FROM forge_pipeline_jobs WHERE id = ?
  )"));
    if (!stmt.s)
      return std::nullopt;

    sqlite3_bind_text (stmt, 1, forge_id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) == SQLITE_ROW)
      return row_to_forge_job (stmt);
    return std::nullopt;
  }

  std::vector<ForgePipelineJob> Database::load_in_flight_forge_pipeline_jobs ()
  {
    std::vector<ForgePipelineJob> jobs;
    if (!db_)
      return jobs;

    Stmt stmt (prepare (R"(
      SELECT id, task_id, status, requirement_json,
             writer_output_json, reviewer_verdict_json,
             feedback, attempt, max_attempts,
             last_code_path, created_at, updated_at
      FROM forge_pipeline_jobs
      WHERE status NOT IN ('promoted','rejected','human_review')
  )"));
    if (!stmt.s)
      return jobs;

    while (sqlite3_step (stmt) == SQLITE_ROW)
      jobs.push_back (row_to_forge_job (stmt));
    return jobs;
  }

  // ---------------------------------------------------------------------------
  // Agent / Capability tables
  // ---------------------------------------------------------------------------

  void Database::ensure_agent_tables ()
  {
    // Tables created in open(); retained for ABI compatibility only.
  }

  std::vector<Database::AgentRow> Database::load_enabled_agents ()
  {
    std::vector<AgentRow> rows;
    if (!db_)
      return rows;

    Stmt stmt (prepare (R"(
      SELECT id, role, binary_path, manifest
      FROM agents WHERE enabled = 1
  )"));
    if (!stmt.s)
      return rows;

    while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      AgentRow row;
      row.id = column_text_or_empty (stmt, 0);
      row.role = column_text_or_empty (stmt, 1);
      row.binary_path = column_text_or_empty (stmt, 2);
      row.manifest = column_text_or_empty (stmt, 3);
      rows.push_back (std::move (row));
    }
    return rows;
  }

  void Database::insert_agent (const std::string &id, const std::string &role,
                               const std::string &binary_path,
                               const std::string &manifest)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
      INSERT OR REPLACE INTO agents
          (id, role, binary_path, manifest, approved_by, approved_at, enabled)
      VALUES (?, ?, ?, ?, 'forge', ?, 1)
  )"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, role.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, binary_path.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4, manifest.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 5, now_unix ());

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] insert_agent: {}", sqlite3_errmsg (db_));
  }

  void Database::insert_capability (const std::string &agent_id,
                                    const std::string &method,
                                    const std::string &description,
                                    const std::string &input_schema)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
      INSERT OR REPLACE INTO capabilities
          (agent_id, method, description, input_schema)
      VALUES (?, ?, ?, ?)
  )"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, agent_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, method.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, description.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4, input_schema.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] insert_capability: {}", sqlite3_errmsg (db_));
  }

  // ---------------------------------------------------------------------------
  // HumanReview table
  // ---------------------------------------------------------------------------

  void Database::insert_human_review (const std::string &id,
                                      const std::string &reason,
                                      const std::string &artifacts,
                                      const std::string &forge_id)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
      INSERT INTO human_reviews (id, forge_id, reason, artifacts, status)
      VALUES (?, ?, ?, ?, 'pending')
  )"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, forge_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, reason.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4, artifacts.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] insert_human_review: {}",
                     sqlite3_errmsg (db_));
  }

  // ---------------------------------------------------------------------------
  // AccessKey table (ADR-020)
  // ---------------------------------------------------------------------------

  void Database::insert_access_key (const AccessKey &key)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
      INSERT INTO access_keys
          (id, key, key_hash, key_salt, description, role,
           created_at, expires_at, last_used_at, revoked_at, revoked_reason)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  )"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, key.id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, key.key.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, key.key_hash.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4, key.key_salt.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 5, key.description.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 6, key.role.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 7, key.created_at ? key.created_at : now_unix ());
    bind_optional_int64 (stmt, 8, key.expires_at);
    bind_optional_int64 (stmt, 9, key.last_used_at);
    bind_optional_int64 (stmt, 10, key.revoked_at);
    if (key.revoked_reason)
      sqlite3_bind_text (stmt, 11, key.revoked_reason->c_str (), -1,
                         SQLITE_TRANSIENT);
    else
      sqlite3_bind_null (stmt, 11);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] insert_access_key: {}", sqlite3_errmsg (db_));
  }

  void Database::revoke_access_key (const std::string &id,
                                    const std::string &reason)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (
      "UPDATE access_keys SET revoked_at=?, revoked_reason=? WHERE id=?"));
    if (!stmt.s)
      return;

    sqlite3_bind_int64 (stmt, 1, now_unix ());
    sqlite3_bind_text (stmt, 2, reason.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] revoke_access_key: {}", sqlite3_errmsg (db_));
  }

  void Database::touch_access_key (const std::string &id)
  {
    if (!db_)
      return;
    Stmt stmt (prepare ("UPDATE access_keys SET last_used_at=? WHERE id=?"));
    if (!stmt.s)
      return;

    sqlite3_bind_int64 (stmt, 1, now_unix ());
    sqlite3_bind_text (stmt, 2, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] touch_access_key: {}", sqlite3_errmsg (db_));
  }

  std::vector<Database::AccessKey> Database::load_active_access_keys ()
  {
    std::vector<AccessKey> keys;
    if (!db_)
      return keys;

    Stmt stmt (prepare (R"(
      SELECT id, key, key_hash, key_salt, description, role,
             created_at, expires_at, last_used_at, revoked_at, revoked_reason
      FROM access_keys
      WHERE revoked_at IS NULL
        AND (expires_at IS NULL OR expires_at > unixepoch())
  )"));
    if (!stmt.s)
      return keys;

    while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      AccessKey k;
      k.id = column_text_or_empty (stmt, 0);
      k.key = column_text_or_empty (stmt, 1);
      k.key_hash = column_text_or_empty (stmt, 2);
      k.key_salt = column_text_or_empty (stmt, 3);
      k.description = column_text_or_empty (stmt, 4);
      k.role = column_text_or_empty (stmt, 5);
      k.created_at = sqlite3_column_int64 (stmt, 6);
      k.expires_at = column_int64_opt (stmt, 7);
      k.last_used_at = column_int64_opt (stmt, 8);
      k.revoked_at = column_int64_opt (stmt, 9);
      auto reason = column_text_or_empty (stmt, 10);
      if (!reason.empty ())
        k.revoked_reason = reason;
      keys.push_back (std::move (k));
    }
    return keys;
  }

} // namespace agentos
