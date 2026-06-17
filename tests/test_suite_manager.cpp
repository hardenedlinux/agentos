#include <gtest/gtest.h>
#include <sqlite3.h>
#include <agentos/database.h>
#include <agentos/home_init.h>
#include <agentos/suite_manager.h>
#include <agentos/types.h>
#include <cstdio>

using namespace agentos;

namespace
{

  class SuiteManagerTest : public ::testing::Test
  {
  protected:
    agentos::Database db{ ":memory:" };

    void SetUp () override
    {
      ASSERT_TRUE (db.open ());

      sqlite3 *raw = db.db_handle ();
      ASSERT_NE (raw, nullptr);

      auto exec = [raw] (const char *sql)
      {
        char *err = nullptr;
        int rc = sqlite3_exec (raw, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK)
          {
            std::fprintf (stderr, "SQL error: %s\n", err ? err : "unknown");
            sqlite3_free (err);
            return false;
          }
        if (err)
          sqlite3_free (err);
        return true;
      };

      // Drop and recreate all three tables unconditionally so SetUp owns
      // the schema entirely and is not coupled to whatever db.open() may
      // have already created.
      ASSERT_TRUE (exec ("DROP TABLE IF EXISTS agents;"));
      ASSERT_TRUE (exec ("DROP TABLE IF EXISTS suite_status;"));
      ASSERT_TRUE (exec ("DROP TABLE IF EXISTS suite_purchases;"));

      ASSERT_TRUE (exec (
        "CREATE TABLE agents ("
        "  ref           TEXT NOT NULL,"
        "  version       TEXT NOT NULL,"
        "  role          TEXT NOT NULL,"
        "  binary_path   TEXT NOT NULL,"
        "  enabled       INTEGER NOT NULL DEFAULT 1,"
        "  registered_at INTEGER NOT NULL,"
        "  PRIMARY KEY (ref, version)"
        ");"));

      ASSERT_TRUE (exec (
        "CREATE TABLE suite_status ("
        "  suite_id    TEXT PRIMARY KEY,"
        "  version     TEXT NOT NULL,"
        "  available   INTEGER NOT NULL DEFAULT 1,"
        "  checked_at  INTEGER NOT NULL"
        ");"));

      ASSERT_TRUE (exec (
        "CREATE TABLE suite_purchases ("
        "  suite_id         TEXT NOT NULL,"
        "  version          TEXT NOT NULL,"
        "  subscription_key TEXT NOT NULL,"
        "  purchased_at     INTEGER NOT NULL,"
        "  expires_at       INTEGER,"
        "  PRIMARY KEY (suite_id, version)"
        ");"));
    }
  };

  TEST_F (SuiteManagerTest, resolve_ref_no_version_returns_latest)
  {
    char *err = nullptr;
    auto *raw = db.db_handle ();

    ASSERT_EQ (sqlite3_exec (
                 raw,
                 "INSERT INTO agents(ref,version,role,binary_path,enabled,registered_at) "
                 "VALUES('marketplace:test:abc','1.0.0','worker','/path/v1',1,1000);",
                 nullptr, nullptr, &err),
               SQLITE_OK)
      << (err ? err : "no error");

    ASSERT_EQ (sqlite3_exec (
                 raw,
                 "INSERT INTO agents(ref,version,role,binary_path,enabled,registered_at) "
                 "VALUES('marketplace:test:abc','2.0.0','worker','/path/v2',1,2000);",
                 nullptr, nullptr, &err),
               SQLITE_OK)
      << (err ? err : "no error");

    agentos::SuiteManager mgr (db);
    auto result = mgr.resolve_ref ("marketplace:test:abc");
    ASSERT_TRUE (result.has_value ()) << result.error ();
    EXPECT_EQ (*result, "/path/v2");
  }

  TEST_F (SuiteManagerTest, resolve_ref_with_version)
  {
    char *err = nullptr;
    auto *raw = db.db_handle ();
    ASSERT_EQ (sqlite3_exec (
                 raw,
                 "INSERT INTO agents(ref,version,role,binary_path,enabled,registered_at) "
                 "VALUES('builtin::planning','1.0','adviser','/builtins/plan',1,500);",
                 nullptr, nullptr, &err),
               SQLITE_OK);
    agentos::SuiteManager mgr (db);
    auto res = mgr.resolve_ref ("builtin::planning", "1.0");
    ASSERT_TRUE (res.has_value ());
    EXPECT_EQ (*res, "/builtins/plan");
  }

  TEST_F (SuiteManagerTest, resolve_ref_unknown_returns_error)
  {
    agentos::SuiteManager mgr (db);
    auto res = mgr.resolve_ref ("no::such::ref");
    ASSERT_FALSE (res.has_value ());
    EXPECT_NE (res.error ().find ("Capability unavailable"), std::string::npos);
  }

  // Tests the "Marketplace unreachable → mark unavailable" path,
  // not the steady-state behaviour.
  TEST_F (SuiteManagerTest, poll_availability_unreachable_marks_all_unavailable)
  {
    char *err = nullptr;
    auto *raw = db.db_handle ();
    ASSERT_EQ (sqlite3_exec (
                 raw,
                 "INSERT INTO suite_status(suite_id,version,available,checked_at) "
                 "VALUES('s1','1.0',1,100);",
                 nullptr, nullptr, &err),
               SQLITE_OK);
    ASSERT_EQ (sqlite3_exec (
                 raw,
                 "INSERT INTO suite_status(suite_id,version,available,checked_at) "
                 "VALUES('s2','2.0',1,100);",
                 nullptr, nullptr, &err),
               SQLITE_OK);

    agentos::SuiteManager mgr (db);
    mgr.poll_availability ();

    // Verify both suites became unavailable and checked_at was updated
    auto s1 = db.load_suite_status ("s1");
    ASSERT_TRUE (s1.has_value ());
    EXPECT_FALSE (s1->available);
    EXPECT_GT (s1->checked_at, 100);

    auto s2 = db.load_suite_status ("s2");
    ASSERT_TRUE (s2.has_value ());
    EXPECT_FALSE (s2->available);
    EXPECT_GT (s2->checked_at, 100);
  }

} // anonymous namespace
