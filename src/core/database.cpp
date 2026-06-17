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
#include "agentos/database.h"
#include "agentos/cred_vault.h"
#include "agentos/forge_pipeline_job.h"
#include "agentos/home_init.h"
#include "agentos/secure_enclave.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <rapidjson/document.h>
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

  static std::string generate_uuid ()
  {
    std::ifstream f ("/proc/sys/kernel/random/uuid");
    if (!f)
    {
      spdlog::error (
        "[database] generate_uuid: cannot open /proc/sys/kernel/random/uuid");
      return {};
    }
    std::string u;
    std::getline (f, u);
    return u;
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
        created_at INTEGER NOT NULL DEFAULT 0,
        updated_at INTEGER NOT NULL
    );
    CREATE TABLE IF NOT EXISTS tasks (
        id           TEXT PRIMARY KEY,
        job_id       TEXT NOT NULL,
        agent_id     TEXT,
        method       TEXT,
        params       TEXT,
        status       TEXT DEFAULT 'pending',
        result       TEXT,
        description  TEXT,
        step_order   INTEGER,
        started_at   INTEGER,
        completed_at INTEGER,
        error        TEXT
    );
    CREATE TABLE IF NOT EXISTS worker_runs (
        run_id     TEXT PRIMARY KEY,
        worker_id  TEXT NOT NULL,
        pid        INTEGER NOT NULL,
        started_at INTEGER NOT NULL,
        ended_at   INTEGER,
        exit_code  INTEGER,
        status     INTEGER NOT NULL DEFAULT 0,
        layer_path TEXT NOT NULL,
        log_path   TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS forge_pipeline_jobs (
        id                    TEXT PRIMARY KEY,
        task_id               TEXT NOT NULL,
        status                INTEGER NOT NULL DEFAULT 0,
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
        forge_id    TEXT,
        reason      TEXT NOT NULL,
        artifacts   TEXT NOT NULL,
        status      TEXT DEFAULT 'pending',
        decision    TEXT,
        reviewed_at INTEGER,
        type        TEXT,
        job_id      TEXT,
        created_at  INTEGER NOT NULL DEFAULT 0
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
    CREATE TABLE IF NOT EXISTS timer_tasks (
        id          TEXT PRIMARY KEY,
        interval_s  INTEGER NOT NULL,
        next_fire   INTEGER NOT NULL,
        target      TEXT NOT NULL,
        payload_json TEXT NOT NULL,
        enabled     INTEGER NOT NULL DEFAULT 1,
        created_at  INTEGER NOT NULL
    );
    -- ADR-028 credential tables
    CREATE TABLE IF NOT EXISTS credentials (
        id                   TEXT PRIMARY KEY,
        user_id              TEXT NOT NULL,
        provider             TEXT NOT NULL,
        ciphertext           BLOB NOT NULL,
        nonce                BLOB NOT NULL,
        refresh_ciphertext   BLOB,
        refresh_nonce        BLOB,
        expires_at           INTEGER,
        refresh_expires_at   INTEGER,
        created_at           INTEGER NOT NULL,
        updated_at           INTEGER NOT NULL,
        UNIQUE(user_id, provider)
    );
    CREATE TABLE IF NOT EXISTS credential_grants (
        id          TEXT PRIMARY KEY,
        worker_id   TEXT NOT NULL,
        provider    TEXT NOT NULL,
        suite_id    TEXT,
        granted_at  INTEGER NOT NULL,
        granted_by  TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS credential_audit (
        id            TEXT PRIMARY KEY,
        credential_id TEXT NOT NULL,
        user_id       TEXT NOT NULL,
        worker_id     TEXT NOT NULL,
        job_id        TEXT NOT NULL,
        step_id       TEXT NOT NULL,
        run_id        TEXT NOT NULL,
        action        TEXT NOT NULL,
        reason        TEXT,
        timestamp     INTEGER NOT NULL
    );
  )";
    if (!exec_ddl (schema))
      return false;

    // ADR-029: users table & profile view
    static const char *schema_users = R"(
      CREATE TABLE IF NOT EXISTS users (
          id         TEXT PRIMARY KEY,
          enabled    INTEGER NOT NULL DEFAULT 1,
          created_at INTEGER NOT NULL
      );
      CREATE VIEW IF NOT EXISTS user_profile AS
      SELECT
          u.id                                                      AS user_id,
          u.created_at                                              AS first_seen,
          MAX(j.created_at)                                         AS last_seen,
          COUNT(j.id)                                               AS total_jobs,
          SUM(CASE WHEN j.phase = 'done' THEN 1 ELSE 0 END)        AS successful_jobs,
          SUM(CASE WHEN j.phase = 'failed' THEN 1 ELSE 0 END)      AS failed_jobs,
          GROUP_CONCAT(DISTINCT c.provider)                         AS connected_providers
      FROM users u
      LEFT JOIN jobs        j ON j.user_id = u.id
      LEFT JOIN credentials c ON c.user_id = u.id
      GROUP BY u.id;
    )";
    if (!exec_ddl (schema_users))
      return false;

    // ADR-022: add columns to tasks table (idempotent)
    {
      auto maybe_add_column = [this] (const char *ddl)
      {
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
      maybe_add_column ("ALTER TABLE tasks ADD COLUMN error TEXT");
    }

    // ADR‑025: extend the jobs table with new columns (idempotent)
    {
      auto maybe_add_column = [this] (const char *ddl)
      {
        char *err = nullptr;
        sqlite3_exec (db_, ddl, nullptr, nullptr, &err);
        if (err)
        {
          spdlog::debug ("[database] jobs migration '{}': {}", ddl, err);
          sqlite3_free (err);
        }
      };

      maybe_add_column (
        "ALTER TABLE jobs ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN type TEXT");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN goal TEXT");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN tags TEXT DEFAULT '[]'");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN error TEXT");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN max_iterations INTEGER");
      maybe_add_column (
        "ALTER TABLE jobs ADD COLUMN current_iteration INTEGER");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN max_repairs INTEGER");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN current_repairs INTEGER");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN reviewer_id TEXT");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN acceptance_criteria TEXT");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN last_feedback TEXT");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN timer_id TEXT");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN interval_s INTEGER");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN starts_at INTEGER");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN last_run_at INTEGER");
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN next_run_at INTEGER");
      // ADR-029: add user_id to jobs
      maybe_add_column ("ALTER TABLE jobs ADD COLUMN user_id TEXT NOT NULL DEFAULT '0'");
    }

    // ADR‑025: extend human_reviews with type / job_id
    {
      auto maybe_add_column = [this] (const char *ddl)
      {
        char *err = nullptr;
        sqlite3_exec (db_, ddl, nullptr, nullptr, &err);
        if (err)
        {
          spdlog::debug ("[database] review migration '{}': {}", ddl, err);
          sqlite3_free (err);
        }
      };
      maybe_add_column ("ALTER TABLE human_reviews ADD COLUMN type TEXT");
      maybe_add_column ("ALTER TABLE human_reviews ADD COLUMN job_id TEXT");
      maybe_add_column ("ALTER TABLE human_reviews ADD COLUMN created_at "
                        "INTEGER NOT NULL DEFAULT 0");
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

  static void bind_optional_text (sqlite3_stmt *stmt, int col,
                                  const std::optional<std::string> &val)
  {
    if (val)
      sqlite3_bind_text (stmt, col, val->c_str (), -1, SQLITE_TRANSIENT);
    else
      sqlite3_bind_null (stmt, col);
  }

  static void
  bind_optional_blob (sqlite3_stmt *stmt, int col,
                      const std::optional<std::vector<uint8_t>> &val)
  {
    if (val)
      sqlite3_bind_blob (stmt, col, val->data (),
                         static_cast<int> (val->size ()), SQLITE_TRANSIENT);
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

  static std::optional<std::vector<uint8_t>>
  column_blob_opt (sqlite3_stmt *stmt, int col)
  {
    if (sqlite3_column_type (stmt, col) == SQLITE_NULL)
      return std::nullopt;
    const void *p = sqlite3_column_blob (stmt, col);
    int sz = sqlite3_column_bytes (stmt, col);
    if (!p || sz <= 0)
      return std::nullopt;
    const uint8_t *b = static_cast<const uint8_t *> (p);
    return std::vector<uint8_t> (b, b + sz);
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

  void Database::bind_optional_text (sqlite3_stmt *stmt, int col,
                                     const std::optional<std::string> &val)
  {
    ::agentos::bind_optional_text (stmt, col, val);
  }


  // ========================================================================
  //  row_to_* helpers (file-scope)
  // ========================================================================

  static Job row_to_job (sqlite3_stmt *stmt)
  {
    Job j;
    j.id = column_text_or_empty (stmt, 0);
    j.type = column_text_or_empty (stmt, 1);
    j.goal = column_text_or_empty (stmt, 2);

    // tags — parse JSON array
    std::string tags_str = column_text_or_empty (stmt, 3);
    if (!tags_str.empty ())
    {
      rapidjson::Document d;
      d.Parse (tags_str.c_str ());
      if (d.IsArray ())
      {
        for (const auto &v : d.GetArray ())
        {
          if (v.IsString ())
            j.tags.emplace_back (v.GetString ());
        }
      }
    }

    j.phase = column_text_or_empty (stmt, 4);
    j.created_at = sqlite3_column_int64 (stmt, 5);
    j.updated_at = sqlite3_column_int64 (stmt, 6);

    // error (nullable)
    if (sqlite3_column_type (stmt, 7) != SQLITE_NULL)
      j.error = column_text_or_empty (stmt, 7);

    // ----------------------------------------------------------------------
    // Loop fields (columns 8‑14)
    // Present only when type == loop
    // ----------------------------------------------------------------------
    std::string type = j.type;
    if (type == std::string (db::job_type::loop))
    {
      Loop l;
      l.max_iterations = sqlite3_column_int (stmt, 8);
      l.current_iteration = sqlite3_column_int (stmt, 9);
      l.max_repairs = sqlite3_column_int (stmt, 10);
      l.current_repairs = sqlite3_column_int (stmt, 11);
      l.reviewer_id = column_text_or_empty (stmt, 12);
      l.acceptance_criteria = column_text_or_empty (stmt, 13);
      if (sqlite3_column_type (stmt, 14) != SQLITE_NULL)
        l.last_feedback = column_text_or_empty (stmt, 14);
      j.loop = std::move (l);
    }

    // ----------------------------------------------------------------------
    // Schedule fields (columns 15‑19)
    // Present only when type == scheduled
    // ----------------------------------------------------------------------
    if (type == std::string (db::job_type::scheduled))
    {
      Schedule s;
      s.timer_id = column_text_or_empty (stmt, 15);
      s.interval_s = sqlite3_column_int64 (stmt, 16);
      s.starts_at = column_int64_opt (stmt, 17);
      s.last_run_at = column_int64_opt (stmt, 18);
      s.next_run_at = column_int64_opt (stmt, 19);
      j.schedule = std::move (s);
    }

    return j;
  }

  static Step row_to_step (sqlite3_stmt *stmt)
  {
    Step s;
    s.id = column_text_or_empty (stmt, 0);
    s.job_id = column_text_or_empty (stmt, 1);
    s.step_order = sqlite3_column_int (stmt, 2);
    s.description = column_text_or_empty (stmt, 3);
    s.status = column_text_or_empty (stmt, 4);
    s.started_at = column_int64_opt (stmt, 5);
    s.completed_at = column_int64_opt (stmt, 6);
    if (sqlite3_column_type (stmt, 7) != SQLITE_NULL)
      s.error = column_text_or_empty (stmt, 7);
    return s;
  }

  static HumanReview row_to_human_review (sqlite3_stmt *stmt)
  {
    HumanReview r;
    r.id = column_text_or_empty (stmt, 0);
    r.type = column_text_or_empty (stmt, 1);
    if (sqlite3_column_type (stmt, 2) != SQLITE_NULL)
      r.forge_id = column_text_or_empty (stmt, 2);
    if (sqlite3_column_type (stmt, 3) != SQLITE_NULL)
      r.job_id = column_text_or_empty (stmt, 3);
    r.reason = column_text_or_empty (stmt, 4);
    r.artifacts = column_text_or_empty (stmt, 5);
    r.status = column_text_or_empty (stmt, 6);
    if (sqlite3_column_type (stmt, 7) != SQLITE_NULL)
      r.decision = column_text_or_empty (stmt, 7);
    r.created_at = sqlite3_column_int64 (stmt, 8);
    r.reviewed_at = column_int64_opt (stmt, 9);
    return r;
  }

  static CredentialRow row_to_credential (sqlite3_stmt *stmt)
  {
    CredentialRow c;
    c.id = column_text_or_empty (stmt, 0);
    c.user_id = column_text_or_empty (stmt, 1);
    c.provider = column_text_or_empty (stmt, 2);
    // ciphertext/nonce are NOT NULL in schema; use .value() so a corrupt DB
    // throws std::bad_optional_access rather than silent UB from *nullopt.
    c.ciphertext = column_blob_opt (stmt, 3).value ();
    c.nonce = column_blob_opt (stmt, 4).value ();
    c.refresh_ciphertext = column_blob_opt (stmt, 5);
    c.refresh_nonce = column_blob_opt (stmt, 6);
    c.expires_at = column_int64_opt (stmt, 7);
    c.refresh_expires_at = column_int64_opt (stmt, 8);
    c.created_at = sqlite3_column_int64 (stmt, 9);
    c.updated_at = sqlite3_column_int64 (stmt, 10);
    return c;
  }

  static GrantRow row_to_grant (sqlite3_stmt *stmt)
  {
    GrantRow g;
    g.id = column_text_or_empty (stmt, 0);
    g.worker_id = column_text_or_empty (stmt, 1);
    g.provider = column_text_or_empty (stmt, 2);
    if (sqlite3_column_type (stmt, 3) != SQLITE_NULL)
      g.suite_id = column_text_or_empty (stmt, 3);
    g.granted_at = sqlite3_column_int64 (stmt, 4);
    g.granted_by = column_text_or_empty (stmt, 5);
    return g;
  }

  static CredentialAuditRow row_to_audit (sqlite3_stmt *stmt)
  {
    CredentialAuditRow a;
    a.id = column_text_or_empty (stmt, 0);
    a.credential_id = column_text_or_empty (stmt, 1);
    a.user_id = column_text_or_empty (stmt, 2);
    a.worker_id = column_text_or_empty (stmt, 3);
    a.job_id = column_text_or_empty (stmt, 4);
    a.step_id = column_text_or_empty (stmt, 5);
    a.run_id = column_text_or_empty (stmt, 6);
    a.action = column_text_or_empty (stmt, 7);
    if (sqlite3_column_type (stmt, 8) != SQLITE_NULL)
      a.reason = column_text_or_empty (stmt, 8);
    a.timestamp = sqlite3_column_int64 (stmt, 9);
    return a;
  }

  // ========================================================================
  //  ADR‑025 public methods
  // ========================================================================

  // ---------- Job ----------

  void Database::insert_job (const Job &job)
  {
    if (!db_)
      return;

    // serialise tags → JSON array
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartArray ();
    for (const auto &t : job.tags)
      w.String (t.c_str ());
    w.EndArray ();
    std::string tags_json = buf.GetString ();

    Stmt stmt (prepare (R"(
      INSERT INTO jobs (id, type, goal, tags, phase,
                        created_at, updated_at, error,
                        max_iterations, current_iteration,
                        max_repairs, current_repairs,
                        reviewer_id, acceptance_criteria, last_feedback,
                        timer_id, interval_s, starts_at, last_run_at, next_run_at)
      VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
    )"));
    if (!stmt.s)
      return;

    const int64_t ts = now_unix ();
    sqlite3_bind_text (stmt, 1, job.id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, job.type.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, job.goal.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4, tags_json.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 5, job.phase.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 6, job.created_at ? job.created_at : ts);
    sqlite3_bind_int64 (stmt, 7, job.updated_at ? job.updated_at : ts);

    bind_optional_text (stmt, 8, job.error);

    if (job.loop)
    {
      const Loop &l = *job.loop;
      sqlite3_bind_int (stmt, 9, l.max_iterations);
      sqlite3_bind_int (stmt, 10, l.current_iteration);
      sqlite3_bind_int (stmt, 11, l.max_repairs);
      sqlite3_bind_int (stmt, 12, l.current_repairs);
      sqlite3_bind_text (stmt, 13, l.reviewer_id.c_str (), -1,
                         SQLITE_TRANSIENT);
      sqlite3_bind_text (stmt, 14, l.acceptance_criteria.c_str (), -1,
                         SQLITE_TRANSIENT);
      bind_optional_text (stmt, 15, l.last_feedback);
    }
    else
    {
      sqlite3_bind_null (stmt, 9);
      sqlite3_bind_null (stmt, 10);
      sqlite3_bind_null (stmt, 11);
      sqlite3_bind_null (stmt, 12);
      sqlite3_bind_null (stmt, 13);
      sqlite3_bind_null (stmt, 14);
      sqlite3_bind_null (stmt, 15);
    }

    if (job.schedule)
    {
      const Schedule &s = *job.schedule;
      sqlite3_bind_text (stmt, 16, s.timer_id.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 17, s.interval_s);
      bind_optional_int64 (stmt, 18, s.starts_at);
      bind_optional_int64 (stmt, 19, s.last_run_at);
      bind_optional_int64 (stmt, 20, s.next_run_at);
    }
    else
    {
      sqlite3_bind_null (stmt, 16);
      sqlite3_bind_null (stmt, 17);
      sqlite3_bind_null (stmt, 18);
      sqlite3_bind_null (stmt, 19);
      sqlite3_bind_null (stmt, 20);
    }

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] insert_job: {}", sqlite3_errmsg (db_));
  }

  void Database::update_job_phase (const std::string &id,
                                   std::string_view /*old_phase*/,
                                   std::string_view new_phase)
  {
    if (!db_)
      return;
    Stmt stmt (
      prepare ("UPDATE jobs SET phase = ?, updated_at = ? WHERE id = ?"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, new_phase.data (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 2, now_unix ());
    sqlite3_bind_text (stmt, 3, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_job_phase: {}", sqlite3_errmsg (db_));
  }

  void Database::update_job_error (const std::string &id,
                                   const std::string &error)
  {
    if (!db_)
      return;
    Stmt stmt (
      prepare ("UPDATE jobs SET error = ?, updated_at = ? WHERE id = ?"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, error.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 2, now_unix ());
    sqlite3_bind_text (stmt, 3, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_job_error: {}", sqlite3_errmsg (db_));
  }

  std::optional<Job> Database::load_job (const std::string &id)
  {
    if (!db_)
      return std::nullopt;
    Stmt stmt (prepare (R"(
      SELECT id, type, goal, tags, phase,
             created_at, updated_at, error,
             max_iterations, current_iteration,
             max_repairs, current_repairs,
             reviewer_id, acceptance_criteria, last_feedback,
             timer_id, interval_s, starts_at, last_run_at, next_run_at
      FROM jobs WHERE id = ?
    )"));
    if (!stmt.s)
      return std::nullopt;

    sqlite3_bind_text (stmt, 1, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) == SQLITE_ROW)
      return row_to_job (stmt);
    return std::nullopt;
  }

  std::vector<Job>
  Database::load_jobs (std::optional<std::string_view> type_filter,
                       std::optional<std::string_view> phase_filter, int limit,
                       int offset)
  {
    std::vector<Job> jobs;
    if (!db_)
      return jobs;

    std::string where;
    int bind_idx = 1;

    if (type_filter)
    {
      where += " AND type = ?";
    }
    if (phase_filter)
    {
      where += " AND phase = ?";
    }

    std::string sql
      = std::string (
          "SELECT id, type, goal, tags, phase, "
          "created_at, updated_at, error, "
          "max_iterations, current_iteration, "
          "max_repairs, current_repairs, "
          "reviewer_id, acceptance_criteria, last_feedback, "
          "timer_id, interval_s, starts_at, last_run_at, next_run_at "
          "FROM jobs WHERE 1=1")
        + where + " ORDER BY created_at DESC LIMIT ? OFFSET ?";

    Stmt stmt (prepare (sql.c_str ()));
    if (!stmt.s)
      return jobs;

    if (type_filter)
    {
      std::string tmp (*type_filter);
      sqlite3_bind_text (stmt, bind_idx++, tmp.c_str (), -1, SQLITE_TRANSIENT);
    }
    if (phase_filter)
    {
      std::string tmp (*phase_filter);
      sqlite3_bind_text (stmt, bind_idx++, tmp.c_str (), -1, SQLITE_TRANSIENT);
    }

    sqlite3_bind_int (stmt, bind_idx++, limit);
    sqlite3_bind_int (stmt, bind_idx++, offset);

    while (sqlite3_step (stmt) == SQLITE_ROW)
      jobs.push_back (row_to_job (stmt));

    return jobs;
  }

  int Database::count_jobs (std::optional<std::string_view> type_filter,
                            std::optional<std::string_view> phase_filter)
  {
    if (!db_)
      return 0;

    std::string where;
    int bind_idx = 1;

    if (type_filter)
      where += " AND type = ?";
    if (phase_filter)
      where += " AND phase = ?";

    std::string sql
      = std::string ("SELECT COUNT(*) FROM jobs WHERE 1=1") + where;
    Stmt stmt (prepare (sql.c_str ()));
    if (!stmt.s)
      return 0;

    if (type_filter)
    {
      std::string tmp (*type_filter);
      sqlite3_bind_text (stmt, bind_idx++, tmp.c_str (), -1, SQLITE_TRANSIENT);
    }
    if (phase_filter)
    {
      std::string tmp (*phase_filter);
      sqlite3_bind_text (stmt, bind_idx++, tmp.c_str (), -1, SQLITE_TRANSIENT);
    }

    if (sqlite3_step (stmt) == SQLITE_ROW)
      return sqlite3_column_int (stmt, 0);
    return 0;
  }

  void Database::increment_job_iteration (const std::string &id)
  {
    if (!db_)
      return;
    Stmt stmt (
      prepare ("UPDATE jobs SET current_iteration = current_iteration + 1, "
               "updated_at = ? WHERE id = ?"));
    if (!stmt.s)
      return;

    sqlite3_bind_int64 (stmt, 1, now_unix ());
    sqlite3_bind_text (stmt, 2, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] increment_job_iteration: {}",
                     sqlite3_errmsg (db_));
  }

  void Database::update_job_feedback (const std::string &id,
                                      const std::string &feedback)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (
      "UPDATE jobs SET last_feedback = ?, updated_at = ? WHERE id = ?"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, feedback.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 2, now_unix ());
    sqlite3_bind_text (stmt, 3, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_job_feedback: {}",
                     sqlite3_errmsg (db_));
  }

  void Database::increment_job_repairs (const std::string &id)
  {
    if (!db_)
      return;
    Stmt stmt (
      prepare ("UPDATE jobs SET current_repairs = current_repairs + 1, "
               "updated_at = ? WHERE id = ?"));
    if (!stmt.s)
      return;

    sqlite3_bind_int64 (stmt, 1, now_unix ());
    sqlite3_bind_text (stmt, 2, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] increment_job_repairs: {}",
                     sqlite3_errmsg (db_));
  }

  // ---------- Step ----------

  void Database::insert_step (const Step &step)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
      INSERT INTO tasks (id, job_id, step_order, description, status,
                         result, started_at, completed_at, error)
      VALUES (?,?,?,?,?, NULL,?,?,?)
    )"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, step.id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, step.job_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 3, step.step_order);
    sqlite3_bind_text (stmt, 4, step.description.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 5, step.status.c_str (), -1, SQLITE_TRANSIENT);
    bind_optional_int64 (stmt, 6, step.started_at);
    bind_optional_int64 (stmt, 7, step.completed_at);
    bind_optional_text (stmt, 8, step.error);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] insert_step: {}", sqlite3_errmsg (db_));
  }

  void Database::update_step_status (const std::string &id,
                                     std::string_view new_status,
                                     std::optional<std::string> error)
  {
    if (!db_)
      return;

    const int64_t ts = now_unix ();
    const bool is_running = (new_status == db::step_status::running);

    // running → set started_at; done/failed → set completed_at.
    const char *sql = is_running ? "UPDATE tasks SET status = ?, error = ?, "
                                   "started_at  = ? WHERE id = ?"
                                 : "UPDATE tasks SET status = ?, error = ?, "
                                   "completed_at = ? WHERE id = ?";

    Stmt stmt (prepare (sql));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, new_status.data (), -1, SQLITE_TRANSIENT);
    bind_optional_text (stmt, 2, error);
    sqlite3_bind_int64 (stmt, 3, ts);
    sqlite3_bind_text (stmt, 4, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_step_status: {}", sqlite3_errmsg (db_));
  }

  void Database::complete_step (const std::string &id,
                                const std::string &result_json)
  {
    // reuse legacy helper that writes result + sets status=done
    update_step_result (id, result_json);
  }

  std::optional<Step> Database::load_step (const std::string &id)
  {
    if (!db_)
      return std::nullopt;
    Stmt stmt (prepare (R"(
      SELECT id, job_id, step_order, description, status,
             started_at, completed_at, error
      FROM tasks WHERE id = ?
    )"));
    if (!stmt.s)
      return std::nullopt;

    sqlite3_bind_text (stmt, 1, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) == SQLITE_ROW)
      return row_to_step (stmt);
    return std::nullopt;
  }

  std::vector<Step> Database::load_steps_for_job (const std::string &job_id)
  {
    std::vector<Step> steps;
    if (!db_)
      return steps;

    Stmt stmt (prepare (R"(
      SELECT id, job_id, step_order, description, status,
             started_at, completed_at, error
      FROM tasks WHERE job_id = ? ORDER BY step_order ASC
    )"));
    if (!stmt.s)
      return steps;

    sqlite3_bind_text (stmt, 1, job_id.c_str (), -1, SQLITE_TRANSIENT);

    while (sqlite3_step (stmt) == SQLITE_ROW)
      steps.push_back (row_to_step (stmt));
    return steps;
  }

  std::optional<std::string>
  Database::load_step_result_opt (const std::string &step_id)
  {
    if (!db_)
      return std::nullopt;
    Stmt stmt (prepare ("SELECT result FROM tasks WHERE id = ?"));
    if (!stmt.s)
      return std::nullopt;

    sqlite3_bind_text (stmt, 1, step_id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      if (sqlite3_column_type (stmt, 0) == SQLITE_NULL)
        return std::nullopt;
      return column_text_or_empty (stmt, 0);
    }
    return std::nullopt;
  }

  // ---------- HumanReview ----------

  void Database::insert_human_review (const HumanReview &review)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
      INSERT INTO human_reviews
          (id, type, forge_id, job_id, reason, artifacts,
           status, decision, created_at, reviewed_at)
      VALUES (?,?,?,?,?,?,?,?,?,?)
    )"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, review.id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, review.type.c_str (), -1, SQLITE_TRANSIENT);
    bind_optional_text (stmt, 3, review.forge_id);
    bind_optional_text (stmt, 4, review.job_id);
    sqlite3_bind_text (stmt, 5, review.reason.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 6, review.artifacts.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 7, review.status.c_str (), -1, SQLITE_TRANSIENT);
    bind_optional_text (stmt, 8, review.decision);
    sqlite3_bind_int64 (stmt, 9,
                        review.created_at ? review.created_at : now_unix ());
    bind_optional_int64 (stmt, 10, review.reviewed_at);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] insert_human_review: {}",
                     sqlite3_errmsg (db_));
  }

  void Database::update_review_status (const std::string &id,
                                       std::string_view status,
                                       const std::string &decision)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (
      "UPDATE human_reviews SET status = ?, decision = ?, reviewed_at = ? "
      "WHERE id = ?"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, status.data (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, decision.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 3, now_unix ());
    sqlite3_bind_text (stmt, 4, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_review_status: {}",
                     sqlite3_errmsg (db_));
  }

  std::optional<HumanReview> Database::load_human_review (const std::string &id)
  {
    if (!db_)
      return std::nullopt;
    Stmt stmt (prepare (R"(
      SELECT id, type, forge_id, job_id, reason, artifacts,
             status, decision, created_at, reviewed_at
      FROM human_reviews WHERE id = ?
    )"));
    if (!stmt.s)
      return std::nullopt;

    sqlite3_bind_text (stmt, 1, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) == SQLITE_ROW)
      return row_to_human_review (stmt);
    return std::nullopt;
  }

  std::vector<HumanReview>
  Database::load_human_reviews (std::optional<std::string_view> type_filter,
                                std::optional<std::string_view> status_filter)
  {
    std::vector<HumanReview> revs;
    if (!db_)
      return revs;

    std::string where;
    int bind_idx = 1;

    if (type_filter)
      where += " AND type = ?";
    if (status_filter)
      where += " AND status = ?";

    std::string sql
      = std::string ("SELECT id, type, forge_id, job_id, reason, artifacts, "
                     "status, decision, created_at, reviewed_at "
                     "FROM human_reviews WHERE 1=1")
        + where + " ORDER BY created_at DESC";

    Stmt stmt (prepare (sql.c_str ()));
    if (!stmt.s)
      return revs;

    if (type_filter)
    {
      std::string tmp (*type_filter);
      sqlite3_bind_text (stmt, bind_idx++, tmp.c_str (), -1, SQLITE_TRANSIENT);
    }
    if (status_filter)
    {
      std::string tmp (*status_filter);
      sqlite3_bind_text (stmt, bind_idx++, tmp.c_str (), -1, SQLITE_TRANSIENT);
    }

    while (sqlite3_step (stmt) == SQLITE_ROW)
      revs.push_back (row_to_human_review (stmt));

    return revs;
  }

  // ---------- Crash recovery helpers ----------

  std::vector<Job> Database::load_active_jobs ()
  {
    std::vector<Job> jobs;
    if (!db_)
      return jobs;

    Stmt stmt (prepare (
      "SELECT id, type, goal, tags, phase, created_at, updated_at, error, "
      "max_iterations, current_iteration, max_repairs, current_repairs, "
      "reviewer_id, acceptance_criteria, last_feedback, "
      "timer_id, interval_s, starts_at, last_run_at, next_run_at "
      "FROM jobs "
      "WHERE phase NOT IN (?, ?, ?) "
      "ORDER BY created_at"));
    if (!stmt.s)
      return jobs;

    sqlite3_bind_text (stmt, 1, db::job_phase::done.data (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, db::job_phase::failed.data (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, db::job_phase::human_review.data (), -1,
                       SQLITE_TRANSIENT);

    while (sqlite3_step (stmt) == SQLITE_ROW)
      jobs.push_back (row_to_job (stmt));
    return jobs;
  }

  std::vector<Step>
  Database::load_active_steps (const std::vector<std::string> &job_ids)
  {
    std::vector<Step> steps;
    if (!db_ || job_ids.empty ())
      return steps;

    // build IN clause
    std::string placeholders;
    for (size_t i = 0; i < job_ids.size (); ++i)
    {
      if (i > 0)
        placeholders += ", ";
      placeholders += "?";
    }

    std::string sql
      = std::string ("SELECT id, job_id, step_order, description, status, "
                     "started_at, completed_at, error "
                     "FROM tasks WHERE job_id IN (")
        + placeholders + ") AND status != ? ORDER BY step_order ASC";

    Stmt stmt (prepare (sql.c_str ()));
    if (!stmt.s)
      return steps;

    for (size_t i = 0; i < job_ids.size (); ++i)
      sqlite3_bind_text (stmt, static_cast<int> (i + 1), job_ids[i].c_str (),
                         -1, SQLITE_TRANSIENT);

    std::string done_status = std::string (db::step_status::done);
    sqlite3_bind_text (stmt, static_cast<int> (job_ids.size () + 1),
                       done_status.c_str (), -1, SQLITE_TRANSIENT);

    while (sqlite3_step (stmt) == SQLITE_ROW)
      steps.push_back (row_to_step (stmt));
    return steps;
  }

  // ========================================================================
  //  Existing (original) methods — kept unchanged below
  // ========================================================================

  // ---------------------------------------------------------------------------
  // Job table (original)
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

  void Database::update_job_user (const std::string &job_id,
                                  const std::string &user_id)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (
      "UPDATE jobs SET user_id = ?, updated_at = ? WHERE id = ?"));
    if (!stmt.s)
      return;
    sqlite3_bind_text (stmt, 1, user_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 2, now_unix ());
    sqlite3_bind_text (stmt, 3, job_id.c_str (), -1, SQLITE_TRANSIENT);
    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_job_user: {}", sqlite3_errmsg (db_));
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
    sqlite3_bind_text (stmt, 2, job_id.value ().c_str (), -1, SQLITE_TRANSIENT);
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
    {
      if (sqlite3_column_type (stmt, 0) == SQLITE_NULL)
        return "";
      return column_text_or_empty (stmt, 0);
    }
    return "";
  }

  void Database::update_step_result (const std::string &step_id,
                                     const std::string &result_json)
  {
    if (!db_)
      return;
    std::string done_status = std::string (db::step_status::done);
    Stmt stmt (
      prepare ("UPDATE tasks SET result = ?, status = ?, completed_at = ? "
               "WHERE id = ?"));
    if (!stmt.s)
      return;
    sqlite3_bind_text (stmt, 1, result_json.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, done_status.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 3, now_unix ());
    sqlite3_bind_text (stmt, 4, step_id.c_str (), -1, SQLITE_TRANSIENT);
    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] update_step_result: {}", sqlite3_errmsg (db_));
  }

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
    run.status = static_cast<WorkerStatus> (sqlite3_column_int (stmt, 6));
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

    sqlite3_bind_int (stmt, 7, static_cast<int> (run.status));
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

    sqlite3_bind_int (stmt, 3, static_cast<int> (run.status));
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
      FROM worker_runs WHERE status = 0
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
    Stmt stmt (prepare ("UPDATE worker_runs SET status=3, ended_at=? "
                        "WHERE status=0"));
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
    fj.status = static_cast<ForgeStatus> (sqlite3_column_int (stmt, 2));
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
    sqlite3_bind_int (stmt, 3, static_cast<int> (job.status));
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

    sqlite3_bind_int (stmt, 1, static_cast<int> (job.status));
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
                                                   ForgeStatus status)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (
      "UPDATE forge_pipeline_jobs SET status=?, updated_at=? WHERE id=?"));
    if (!stmt.s)
      return;

    sqlite3_bind_int (stmt, 1, static_cast<int> (status));
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
      WHERE status NOT IN (2, 3, 4)
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

  std::vector<Database::CapabilityRow> Database::load_capabilities ()
  {
    std::vector<CapabilityRow> rows;
    if (!db_)
      return rows;

    Stmt stmt (prepare (R"(
      SELECT capabilities.agent_id, capabilities.method,
             capabilities.description, capabilities.input_schema
      FROM capabilities
      JOIN agents ON agents.id = capabilities.agent_id
      WHERE agents.enabled = 1
  )"));
    if (!stmt.s)
      return rows;

    while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      CapabilityRow row;
      row.agent_id = column_text_or_empty (stmt, 0);
      row.method = column_text_or_empty (stmt, 1);
      row.description = column_text_or_empty (stmt, 2);
      row.input_schema = column_text_or_empty (stmt, 3);
      rows.push_back (std::move (row));
    }
    return rows;
  }

  // ---------------------------------------------------------------------------
  // HumanReview table (original)
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

  // ---------------------------------------------------------------------------
  // timer_tasks table (ADR-023)
  // ---------------------------------------------------------------------------

  static TaskTarget target_from_string (const std::string &s)
  {
    if (s == "orchestrator")
      return TaskTarget::Orchestrator;
    if (s == "master")
      return TaskTarget::Master;
    return TaskTarget::Gateway;
  }

  static TimerTask row_to_timer_task (sqlite3_stmt *stmt)
  {
    TimerTask t;
    t.id = column_text_or_empty (stmt, 0);
    t.interval_s = sqlite3_column_int64 (stmt, 1);
    t.next_fire = sqlite3_column_int64 (stmt, 2);
    t.target = target_from_string (column_text_or_empty (stmt, 3));
    t.payload_json = column_text_or_empty (stmt, 4);
    t.enabled = sqlite3_column_int (stmt, 5) != 0;
    t.created_at = sqlite3_column_int64 (stmt, 6);
    return t;
  }

  void Database::insert_timer_task (const TimerTask &t)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (
      "INSERT OR IGNORE INTO timer_tasks "
      "(id, interval_s, next_fire, target, payload_json, enabled, created_at) "
      "VALUES (?,?,?,?,?,1,?)"));
    if (!stmt.s)
      return;

    const auto trg = to_string (t.target);
    sqlite3_bind_text (stmt, 1, t.id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 2, t.interval_s);
    sqlite3_bind_int64 (stmt, 3, t.next_fire);
    sqlite3_bind_text (stmt, 4, trg.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 5, t.payload_json.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 6, t.created_at > 0 ? t.created_at : now_unix ());

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] insert_timer_task: {}", sqlite3_errmsg (db_));
  }

  void Database::persist_timer_task (const TimerTask &t)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (
      "INSERT OR REPLACE INTO timer_tasks "
      "(id, interval_s, next_fire, target, payload_json, enabled, created_at) "
      "VALUES (?,?,?,?,?,1,?)"));
    if (!stmt.s)
      return;

    const auto trg = to_string (t.target);
    sqlite3_bind_text (stmt, 1, t.id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 2, t.interval_s);
    sqlite3_bind_int64 (stmt, 3, t.next_fire);
    sqlite3_bind_text (stmt, 4, trg.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 5, t.payload_json.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 6, t.created_at > 0 ? t.created_at : now_unix ());

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] persist_timer_task: {}", sqlite3_errmsg (db_));
  }

  void Database::upsert_timer_task_next_fire (const std::string &id,
                                              int64_t next_fire)
  {
    if (!db_)
      return;
    Stmt stmt (prepare ("UPDATE timer_tasks SET next_fire = ? WHERE id = ?"));
    if (!stmt.s)
      return;

    sqlite3_bind_int64 (stmt, 1, next_fire);
    sqlite3_bind_text (stmt, 2, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] upsert_timer_task_next_fire: {}",
                     sqlite3_errmsg (db_));
  }

  void Database::disable_timer_task (const std::string &id)
  {
    if (!db_)
      return;
    Stmt stmt (prepare ("UPDATE timer_tasks SET enabled = 0 WHERE id = ?"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] disable_timer_task: {}", sqlite3_errmsg (db_));
  }

  bool Database::timer_task_exists (const std::string &id)
  {
    if (!db_)
      return false;
    Stmt stmt (prepare (
      "SELECT COUNT(*) FROM timer_tasks WHERE id = ? AND enabled = 1"));
    if (!stmt.s)
      return false;

    sqlite3_bind_text (stmt, 1, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) == SQLITE_ROW)
      return sqlite3_column_int (stmt, 0) > 0;
    return false;
  }

  std::vector<TimerTask> Database::load_enabled_timer_tasks ()
  {
    std::vector<TimerTask> tasks;
    if (!db_)
      return tasks;

    Stmt stmt (prepare (
      "SELECT id, interval_s, next_fire, target, payload_json, enabled, "
      "created_at FROM timer_tasks WHERE enabled = 1"));
    if (!stmt.s)
      return tasks;

    while (sqlite3_step (stmt) == SQLITE_ROW)
      tasks.push_back (row_to_timer_task (stmt));

    return tasks;
  }

  // ========================================================================
  //  ADR-028 credential methods
  // ========================================================================

  std::expected<std::string, Error> Database::insert_credential (
    const std::string &caller_id,
    const std::string &user_id, const std::string &provider,
    const CipherBlob &token_blob, const std::optional<CipherBlob> &refresh_blob,
    std::optional<int64_t> expires_at)
  {
    if (!db_)
      return std::unexpected (Error{"database not open"});

    // If a credential already exists for (user_id, provider), load its id so
    // we preserve the audit chain (credential_id references must not change).
    {
      auto existing = load_credential (user_id, provider);
      if (existing)
      {
        // Re-encrypt in place: update token blob and expiry only.
        if (!update_credential_token (existing->id, token_blob, expires_at))
          return std::unexpected (Error{"update failed"});
        return existing->id;
      }
    }

    // Use the caller-supplied id — must not be empty.
    if (caller_id.empty ())
      return std::unexpected (Error{"caller_id must not be empty"});

    const int64_t ts = now_unix ();
    // refresh_expires_at is not passed by callers yet; insert NULL for now.
    Stmt stmt (prepare (R"(
          INSERT INTO credentials
              (id, user_id, provider, ciphertext, nonce,
               refresh_ciphertext, refresh_nonce,
               expires_at, refresh_expires_at, created_at, updated_at)
          VALUES (?,?,?,?,?,?,?,?,NULL,?,?)
      )"));
    if (!stmt.s)
      return std::unexpected (Error{"prepare failed"});

    sqlite3_bind_text (stmt, 1, caller_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, user_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, provider.c_str (), -1, SQLITE_TRANSIENT);
    bind_optional_blob (stmt, 4, token_blob.ciphertext);
    bind_optional_blob (stmt, 5, token_blob.nonce);
    if (refresh_blob)
    {
      bind_optional_blob (stmt, 6, refresh_blob->ciphertext);
      bind_optional_blob (stmt, 7, refresh_blob->nonce);
    }
    else
    {
      sqlite3_bind_null (stmt, 6);
      sqlite3_bind_null (stmt, 7);
    }
    bind_optional_int64 (stmt, 8, expires_at);
    sqlite3_bind_int64 (stmt, 9, ts);
    sqlite3_bind_int64 (stmt, 10, ts);

    if (sqlite3_step (stmt) != SQLITE_DONE)
    {
      spdlog::error ("[database] insert_credential: {}", sqlite3_errmsg (db_));
      return std::unexpected (Error{"insert failed"});
    }
    return caller_id;
  }

  std::optional<CredentialRow>
  Database::load_credential (const std::string &user_id,
                             const std::string &provider)
  {
    if (!db_)
      return std::nullopt;
    Stmt stmt (prepare (R"(
          SELECT id, user_id, provider, ciphertext, nonce,
                 refresh_ciphertext, refresh_nonce,
                 expires_at, refresh_expires_at, created_at, updated_at
          FROM credentials WHERE user_id=? AND provider=?
      )"));
    if (!stmt.s)
      return std::nullopt;

    sqlite3_bind_text (stmt, 1, user_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, provider.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) == SQLITE_ROW)
      return row_to_credential (stmt);
    return std::nullopt;
  }

  bool Database::update_credential_token (const std::string &id,
                                          const CipherBlob &blob,
                                          std::optional<int64_t> expires_at)
  {
    if (!db_)
      return false;
    Stmt stmt (prepare (R"(
          UPDATE credentials SET ciphertext=?, nonce=?, expires_at=?, updated_at=?
          WHERE id=?
      )"));
    if (!stmt.s)
      return false;

    bind_optional_blob (stmt, 1, blob.ciphertext);
    bind_optional_blob (stmt, 2, blob.nonce);
    bind_optional_int64 (stmt, 3, expires_at);
    sqlite3_bind_int64 (stmt, 4, now_unix ());
    sqlite3_bind_text (stmt, 5, id.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
    {
      spdlog::error ("[database] update_credential_token: {}",
                     sqlite3_errmsg (db_));
      return false;
    }
    return true;
  }

  std::vector<CredentialRow> Database::load_credentials_by_user (const std::string &user_id)
  {
    std::vector<CredentialRow> rows;
    if (!db_)
      return rows;
    Stmt stmt (prepare (R"(
          SELECT id, user_id, provider, ciphertext, nonce,
                 refresh_ciphertext, refresh_nonce,
                 expires_at, refresh_expires_at, created_at, updated_at
          FROM credentials WHERE user_id=? ORDER BY created_at ASC
      )"));
    if (!stmt.s)
      return rows;
    sqlite3_bind_text (stmt, 1, user_id.c_str (), -1, SQLITE_TRANSIENT);
    while (sqlite3_step (stmt) == SQLITE_ROW)
      rows.push_back (row_to_credential (stmt));
    return rows;
  }

  std::vector<CredentialRow> Database::load_all_credentials ()
  {
    std::vector<CredentialRow> rows;
    if (!db_)
      return rows;
    Stmt stmt (prepare (R"(
          SELECT id, user_id, provider, ciphertext, nonce,
                 refresh_ciphertext, refresh_nonce,
                 expires_at, refresh_expires_at, created_at, updated_at
          FROM credentials ORDER BY created_at ASC
      )"));
    if (!stmt.s)
      return rows;
    while (sqlite3_step (stmt) == SQLITE_ROW)
      rows.push_back (row_to_credential (stmt));
    return rows;
  }

  bool Database::update_credential_full (const std::string &id,
                                         const CipherBlob &token_blob,
                                         const std::optional<CipherBlob> &refresh_blob,
                                         std::optional<int64_t> expires_at)
  {
    if (!db_)
      return false;
    Stmt stmt (prepare (R"(
          UPDATE credentials SET ciphertext=?, nonce=?, refresh_ciphertext=?, refresh_nonce=?,
                expires_at=?, updated_at=? WHERE id=?
      )"));
    if (!stmt.s)
      return false;
    bind_optional_blob (stmt, 1, token_blob.ciphertext);
    bind_optional_blob (stmt, 2, token_blob.nonce);
    if (refresh_blob)
    {
      bind_optional_blob (stmt, 3, refresh_blob->ciphertext);
      bind_optional_blob (stmt, 4, refresh_blob->nonce);
    }
    else
    {
      sqlite3_bind_null (stmt, 3);
      sqlite3_bind_null (stmt, 4);
    }
    bind_optional_int64 (stmt, 5, expires_at);
    sqlite3_bind_int64 (stmt, 6, now_unix ());
    sqlite3_bind_text (stmt, 7, id.c_str (), -1, SQLITE_TRANSIENT);
    if (sqlite3_step (stmt) != SQLITE_DONE)
    {
      spdlog::error ("[database] update_credential_full: {}",
                     sqlite3_errmsg (db_));
      return false;
    }
    return true;
  }

  bool Database::revoke_credential (const std::string &user_id,
                                    const std::string &provider)
  {
    if (!db_)
      return false;
    Stmt stmt (prepare (R"(
          DELETE FROM credentials WHERE user_id=? AND provider=?
      )"));
    if (!stmt.s)
      return false;

    sqlite3_bind_text (stmt, 1, user_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, provider.c_str (), -1, SQLITE_TRANSIENT);
    if (sqlite3_step (stmt) != SQLITE_DONE)
    {
      spdlog::error ("[database] revoke_credential: {}", sqlite3_errmsg (db_));
      return false;
    }
    int changes = sqlite3_changes (db_);
    return changes > 0;
  }

  std::vector<CredentialRow>
  Database::load_expiring_credentials (int64_t threshold_unix)
  {
    std::vector<CredentialRow> creds;
    if (!db_)
      return creds;

    Stmt stmt (prepare (R"(
          SELECT id, user_id, provider, ciphertext, nonce,
                 refresh_ciphertext, refresh_nonce,
                 expires_at, refresh_expires_at, created_at, updated_at
          FROM credentials
          WHERE expires_at IS NOT NULL AND expires_at < ?
      )"));
    if (!stmt.s)
      return creds;

    sqlite3_bind_int64 (stmt, 1, threshold_unix);

    while (sqlite3_step (stmt) == SQLITE_ROW)
      creds.push_back (row_to_credential (stmt));
    return creds;
  }

  // ---------------------------------------------------------------------------
  //  grants
  // ---------------------------------------------------------------------------

  std::expected<std::string, Error>
  Database::insert_credential_grant (const std::string &worker_id,
                                     const std::string &provider,
                                     const std::string &granted_by)
  {
    if (!db_)
      return std::unexpected (Error{"database not open"});

    // If grant already exists, return its existing id rather than a new UUID.
    {
      auto existing = load_credential_grant (worker_id, provider);
      if (existing)
        return existing->id;
    }

    std::string id = generate_uuid ();
    if (id.empty ())
      return std::unexpected (Error{"uuid generation failed"});

    const int64_t ts = now_unix ();

    Stmt stmt (prepare (R"(
          INSERT INTO credential_grants
              (id, worker_id, provider, granted_at, granted_by)
          VALUES (?,?,?,?,?)
      )"));
    if (!stmt.s)
      return std::unexpected (Error{"prepare failed"});

    sqlite3_bind_text (stmt, 1, id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, worker_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, provider.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 4, ts);
    sqlite3_bind_text (stmt, 5, granted_by.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) != SQLITE_DONE)
    {
      spdlog::error ("[database] insert_credential_grant: {}",
                     sqlite3_errmsg (db_));
      return std::unexpected (Error{"insert failed"});
    }
    return id;
  }

  std::optional<GrantRow>
  Database::load_credential_grant (const std::string &worker_id,
                                   const std::string &provider)
  {
    if (!db_)
      return std::nullopt;
    Stmt stmt (prepare (R"(
          SELECT id, worker_id, provider, suite_id, granted_at, granted_by
          FROM credential_grants WHERE worker_id=? AND provider=?
      )"));
    if (!stmt.s)
      return std::nullopt;

    sqlite3_bind_text (stmt, 1, worker_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, provider.c_str (), -1, SQLITE_TRANSIENT);

    if (sqlite3_step (stmt) == SQLITE_ROW)
      return row_to_grant (stmt);
    return std::nullopt;
  }

  bool Database::revoke_credential_grant (const std::string &grant_id)
  {
    if (!db_)
      return false;
    Stmt stmt (prepare ("DELETE FROM credential_grants WHERE id=?"));
    if (!stmt.s)
      return false;

    sqlite3_bind_text (stmt, 1, grant_id.c_str (), -1, SQLITE_TRANSIENT);
    if (sqlite3_step (stmt) != SQLITE_DONE)
    {
      spdlog::error ("[database] revoke_credential_grant: {}",
                     sqlite3_errmsg (db_));
      return false;
    }
    int changes = sqlite3_changes (db_);
    return changes > 0;
  }

  // ---------------------------------------------------------------------------
  //  audit
  // ---------------------------------------------------------------------------

  void Database::insert_credential_audit (const CredentialAuditRow &row)
  {
    if (!db_)
      return;
    Stmt stmt (prepare (R"(
          INSERT INTO credential_audit
              (id, credential_id, user_id, worker_id, job_id, step_id, run_id,
               action, reason, timestamp)
          VALUES (?,?,?,?,?,?,?,?,?,?)
      )"));
    if (!stmt.s)
      return;

    sqlite3_bind_text (stmt, 1, row.id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, row.credential_id.c_str (), -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 3, row.user_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 4, row.worker_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 5, row.job_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 6, row.step_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 7, row.run_id.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 8, row.action.c_str (), -1, SQLITE_TRANSIENT);
    bind_optional_text (stmt, 9, row.reason);
    sqlite3_bind_int64 (stmt, 10, row.timestamp ? row.timestamp : now_unix ());

    if (sqlite3_step (stmt) != SQLITE_DONE)
      spdlog::error ("[database] insert_credential_audit: {}",
                     sqlite3_errmsg (db_));
  }

  std::vector<CredentialAuditRow>
  Database::load_credential_audit (const std::optional<std::string> &user_id,
                                   const std::optional<std::string> &job_id,
                                   const std::optional<std::string> &provider,
                                   int limit)
  {
    std::vector<CredentialAuditRow> rows;
    if (!db_)
      return rows;

    std::string where;
    int bind_idx = 1;
    if (user_id)
      where += " AND a.user_id = ?";
    if (job_id)
      where += " AND a.job_id = ?";
    // provider filter uses EXISTS so audit rows survive credential revocation
    if (provider)
      where += " AND EXISTS (SELECT 1 FROM credentials c WHERE "
               "c.id=a.credential_id AND c.provider=?)";

    // No INNER JOIN: audit records must be queryable even after a credential
    // is revoked (deleted from credentials table).
    std::string sql
      = std::string ("SELECT a.id, a.credential_id, a.user_id, a.worker_id, "
                     "a.job_id, a.step_id, a.run_id, "
                     "a.action, a.reason, a.timestamp "
                     "FROM credential_audit a "
                     "WHERE 1=1")
        + where + " ORDER BY a.timestamp DESC LIMIT ?";

    Stmt stmt (prepare (sql.c_str ()));
    if (!stmt.s)
      return rows;

    if (user_id)
      sqlite3_bind_text (stmt, bind_idx++, user_id->c_str (), -1,
                         SQLITE_TRANSIENT);
    if (job_id)
      sqlite3_bind_text (stmt, bind_idx++, job_id->c_str (), -1,
                         SQLITE_TRANSIENT);
    if (provider)
      sqlite3_bind_text (stmt, bind_idx++, provider->c_str (), -1,
                         SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, bind_idx++, limit);

    while (sqlite3_step (stmt) == SQLITE_ROW)
      rows.push_back (row_to_audit (stmt));
    return rows;
  }

} // namespace agentos
