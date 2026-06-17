#include "agentos/user_manager.h"
#include "agentos/database.h"
#include "agentos/types.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <sqlite3.h>
#include <sstream>
#include <utility>

namespace agentos
{
  namespace
  {

    // ---------------------------------------------------------------------------
    // helpers
    // ---------------------------------------------------------------------------

    // Shorthand: return an unexpected<Error> without needing to name T.
    // Use as:  return fail("reason");
    // Works because the caller's return type already fixes T.
    static std::unexpected<Error> fail (const char *msg)
    {
      return std::unexpected<Error> (Error{msg});
    }

    static std::string column_text (sqlite3_stmt *stmt, int col)
    {
      const auto *p
        = reinterpret_cast<const char *> (sqlite3_column_text (stmt, col));
      return p ? p : "";
    }

    static int64_t now_unix ()
    {
      return static_cast<int64_t> (
        std::chrono::duration_cast<std::chrono::seconds> (
          std::chrono::system_clock::now ().time_since_epoch ())
          .count ());
    }

    static sqlite3 *get_db (Database &db)
    {
      return db.db_handle ();
    }

  } // anonymous namespace

  // ---------------------------------------------------------------------------
  // construction
  // ---------------------------------------------------------------------------

  UserManager::UserManager (Database &db) : db_ (db) {}

  // ---------------------------------------------------------------------------
  // register_user  (INSERT OR IGNORE, then read back — idempotent)
  // ---------------------------------------------------------------------------

  std::expected<UserRecord, Error>
  UserManager::register_user (std::string_view user_id)
  {
    sqlite3 *db = get_db (db_);
    if (!db)
      return fail ("database not open");

    // Insert OR IGNORE — does nothing if the row already exists.
    {
      const char *sql = R"(
        INSERT OR IGNORE INTO users (id, enabled, created_at)
        VALUES (?1, 1, ?2)
    )";
      sqlite3_stmt *s = nullptr;
      if (sqlite3_prepare_v2 (db, sql, -1, &s, nullptr) != SQLITE_OK)
        return fail ("prepare insert failed");
      std::unique_ptr<sqlite3_stmt, decltype (&sqlite3_finalize)> stmt (
        s, sqlite3_finalize);
      sqlite3_bind_text (stmt.get (), 1, user_id.data (),
                         static_cast<int> (user_id.size ()), SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt.get (), 2, now_unix ());
      if (sqlite3_step (stmt.get ()) != SQLITE_DONE)
        return fail ("insert failed");
    }

    // Read back (row exists regardless of whether we just inserted it).
    {
      const char *sql = R"(
        SELECT id, enabled, created_at
        FROM users
        WHERE id = ?1
    )";
      sqlite3_stmt *s = nullptr;
      if (sqlite3_prepare_v2 (db, sql, -1, &s, nullptr) != SQLITE_OK)
        return fail ("prepare select failed");
      std::unique_ptr<sqlite3_stmt, decltype (&sqlite3_finalize)> stmt (
        s, sqlite3_finalize);
      sqlite3_bind_text (stmt.get (), 1, user_id.data (),
                         static_cast<int> (user_id.size ()), SQLITE_TRANSIENT);
      if (sqlite3_step (stmt.get ()) != SQLITE_ROW)
        return fail ("user not found after insert");

      UserRecord rec;
      rec.id = column_text (stmt.get (), 0);
      rec.enabled = sqlite3_column_int (stmt.get (), 1) != 0;
      rec.created_at = sqlite3_column_int64 (stmt.get (), 2);
      return rec;
    }
  }

  // ---------------------------------------------------------------------------
  // list_users
  // ---------------------------------------------------------------------------

  std::expected<std::vector<UserRecord>, Error>
  UserManager::list_users (std::optional<bool> enabled_filter, int limit,
                           int offset)
  {
    sqlite3 *db = get_db (db_);
    if (!db)
      return fail ("database not open");

    std::string sql = "SELECT id, enabled, created_at FROM users WHERE 1=1";
    if (enabled_filter)
      sql += *enabled_filter ? " AND enabled = 1" : " AND enabled = 0";
    sql += " ORDER BY created_at DESC LIMIT ?1 OFFSET ?2";

    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2 (db, sql.c_str (), -1, &s, nullptr) != SQLITE_OK)
      return fail ("prepare list failed");
    std::unique_ptr<sqlite3_stmt, decltype (&sqlite3_finalize)> stmt (
      s, sqlite3_finalize);
    sqlite3_bind_int (stmt.get (), 1, limit);
    sqlite3_bind_int (stmt.get (), 2, offset);

    std::vector<UserRecord> users;
    int rc = 0;
    while ((rc = sqlite3_step (stmt.get ())) == SQLITE_ROW)
    {
      UserRecord u;
      u.id = column_text (stmt.get (), 0);
      u.enabled = sqlite3_column_int (stmt.get (), 1) != 0;
      u.created_at = sqlite3_column_int64 (stmt.get (), 2);
      users.push_back (std::move (u));
    }
    if (rc != SQLITE_DONE)
      return fail ("list step error");
    return users;
  }

  // ---------------------------------------------------------------------------
  // count_users
  // ---------------------------------------------------------------------------

  std::expected<int, Error>
  UserManager::count_users (std::optional<bool> enabled_filter)
  {
    sqlite3 *db = get_db (db_);
    if (!db)
      return fail ("database not open");

    std::string sql = "SELECT COUNT(*) FROM users WHERE 1=1";
    if (enabled_filter)
      sql += *enabled_filter ? " AND enabled = 1" : " AND enabled = 0";

    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2 (db, sql.c_str (), -1, &s, nullptr) != SQLITE_OK)
      return fail ("prepare count failed");
    std::unique_ptr<sqlite3_stmt, decltype (&sqlite3_finalize)> stmt (
      s, sqlite3_finalize);
    if (sqlite3_step (stmt.get ()) != SQLITE_ROW)
      return 0;
    return sqlite3_column_int (stmt.get (), 0);
  }

  // ---------------------------------------------------------------------------
  // enable_user / disable_user
  // ---------------------------------------------------------------------------

  std::expected<void, Error> UserManager::enable_user (std::string_view user_id)
  {
    sqlite3 *db = get_db (db_);
    if (!db)
      return fail ("database not open");

    const char *sql = "UPDATE users SET enabled = 1 WHERE id = ?1";
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2 (db, sql, -1, &s, nullptr) != SQLITE_OK)
      return fail ("prepare enable failed");
    std::unique_ptr<sqlite3_stmt, decltype (&sqlite3_finalize)> stmt (
      s, sqlite3_finalize);
    sqlite3_bind_text (stmt.get (), 1, user_id.data (),
                       static_cast<int> (user_id.size ()), SQLITE_TRANSIENT);
    if (sqlite3_step (stmt.get ()) != SQLITE_DONE)
      return fail ("enable update failed");
    if (sqlite3_changes (db) == 0)
      return fail ("user not found");
    return {};
  }

  std::expected<void, Error>
  UserManager::disable_user (std::string_view user_id)
  {
    sqlite3 *db = get_db (db_);
    if (!db)
      return fail ("database not open");

    const char *sql = "UPDATE users SET enabled = 0 WHERE id = ?1";
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2 (db, sql, -1, &s, nullptr) != SQLITE_OK)
      return fail ("prepare disable failed");
    std::unique_ptr<sqlite3_stmt, decltype (&sqlite3_finalize)> stmt (
      s, sqlite3_finalize);
    sqlite3_bind_text (stmt.get (), 1, user_id.data (),
                       static_cast<int> (user_id.size ()), SQLITE_TRANSIENT);
    if (sqlite3_step (stmt.get ()) != SQLITE_DONE)
      return fail ("disable update failed");
    if (sqlite3_changes (db) == 0)
      return fail ("user not found");
    return {};
  }

  // ---------------------------------------------------------------------------
  // validate_user  (hot path called on every job.submit)
  // ADR-029: absent and disabled both return the same error — no distinction.
  // ---------------------------------------------------------------------------

  std::expected<void, Error>
  UserManager::validate_user (std::string_view user_id)
  {
    sqlite3 *db = get_db (db_);
    if (!db)
      return fail ("database not open");

    const char *sql = "SELECT enabled FROM users WHERE id = ?1";
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2 (db, sql, -1, &s, nullptr) != SQLITE_OK)
      return fail ("prepare validate failed");
    std::unique_ptr<sqlite3_stmt, decltype (&sqlite3_finalize)> stmt (
      s, sqlite3_finalize);
    sqlite3_bind_text (stmt.get (), 1, user_id.data (),
                       static_cast<int> (user_id.size ()), SQLITE_TRANSIENT);

    int rc = sqlite3_step (stmt.get ());
    if (rc != SQLITE_ROW)
      return fail ("not found"); // -32020
    if (sqlite3_column_int (stmt.get (), 0) == 0)
      return fail ("not found"); // -32020 (same — no distinction)
    return {};
  }

  // ---------------------------------------------------------------------------
  // get_profile  (queries user_profile view)
  // ---------------------------------------------------------------------------

  std::expected<UserProfile, Error>
  UserManager::get_profile (std::string_view user_id)
  {
    sqlite3 *db = get_db (db_);
    if (!db)
      return fail ("database not open");

    const char *sql = R"(
      SELECT user_id, first_seen, last_seen,
             total_jobs, successful_jobs, failed_jobs,
             connected_providers
      FROM user_profile WHERE user_id = ?1
  )";
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2 (db, sql, -1, &s, nullptr) != SQLITE_OK)
      return fail ("prepare profile failed");
    std::unique_ptr<sqlite3_stmt, decltype (&sqlite3_finalize)> stmt (
      s, sqlite3_finalize);
    sqlite3_bind_text (stmt.get (), 1, user_id.data (),
                       static_cast<int> (user_id.size ()), SQLITE_TRANSIENT);

    if (sqlite3_step (stmt.get ()) != SQLITE_ROW)
      return fail ("profile not found");

    UserProfile p;
    p.user_id = column_text (stmt.get (), 0);
    p.first_seen = sqlite3_column_int64 (stmt.get (), 1);
    if (sqlite3_column_type (stmt.get (), 2) != SQLITE_NULL)
      p.last_seen = sqlite3_column_int64 (stmt.get (), 2);
    else
      p.last_seen = std::nullopt;
    p.total_jobs = sqlite3_column_int (stmt.get (), 3);
    p.successful_jobs = sqlite3_column_int (stmt.get (), 4);
    p.failed_jobs = sqlite3_column_int (stmt.get (), 5);

    // Parse GROUP_CONCAT result ("shopify,amazon,...")
    std::string providers = column_text (stmt.get (), 6);
    if (!providers.empty ())
    {
      std::stringstream ss (providers);
      std::string tok;
      while (std::getline (ss, tok, ','))
        if (!tok.empty ())
          p.connected_providers.push_back (std::move (tok));
    }
    return p;
  }

} // namespace agentos
