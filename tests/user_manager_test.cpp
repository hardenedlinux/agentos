#include <gtest/gtest.h>

#include "agentos/database.h"
#include "agentos/user_manager.h"
#include <cstdint>
#include <memory>
#include <sqlite3.h>

namespace
{

  class UserManagerTest : public ::testing::Test
  {
  protected:
    void SetUp () override
    {
      db_ = std::make_unique<agentos::Database> (":memory:");
      ASSERT_TRUE (db_->open ());
    }

    void TearDown () override
    {
      db_->close ();
      db_.reset ();
    }

    agentos::UserManager makeManager ()
    {
      return agentos::UserManager{*db_};
    }

    std::unique_ptr<agentos::Database> db_;
  };

} // anonymous namespace

// -----------------------------------------------------------------------
// 1. seed_default_user
// -----------------------------------------------------------------------
TEST_F (UserManagerTest, seed_default_user_exists_after_schema)
{
  sqlite3 *db = db_->db_handle ();
  ASSERT_NE (db, nullptr);
  char *err = nullptr;
  int rc
    = sqlite3_exec (db,
                    "INSERT OR IGNORE INTO users (id, enabled, created_at) "
                    "VALUES ('0', 1, unixepoch())",
                    nullptr, nullptr, &err);
  ASSERT_EQ (rc, SQLITE_OK) << (err ? err : "");
  if (err)
    sqlite3_free (err);

  auto mgr = makeManager ();
  auto ok = mgr.validate_user ("0");
  EXPECT_TRUE (ok.has_value ());
}

// -----------------------------------------------------------------------
// 2. register_user
// -----------------------------------------------------------------------
TEST_F (UserManagerTest, register_new_user)
{
  auto mgr = makeManager ();
  auto res = mgr.register_user ("alice");
  ASSERT_TRUE (res.has_value ());
  EXPECT_EQ (res->id, "alice");
  EXPECT_TRUE (res->enabled);
  EXPECT_GT (res->created_at, 0);

  // subsequent call is idempotent
  auto res2 = mgr.register_user ("alice");
  ASSERT_TRUE (res2.has_value ());
  EXPECT_EQ (res2->id, "alice");
  EXPECT_EQ (res2->created_at, res->created_at);
}

// -----------------------------------------------------------------------
// 3. validate_user
// -----------------------------------------------------------------------
TEST_F (UserManagerTest, validate_unknown_user)
{
  auto mgr = makeManager ();
  auto res = mgr.validate_user ("ghost");
  ASSERT_FALSE (res.has_value ());
  // error must be non-empty; the ADR prescribes -32020
}

TEST_F (UserManagerTest, validate_disabled_user)
{
  auto mgr = makeManager ();
  ASSERT_TRUE (mgr.register_user ("bob").has_value ());
  ASSERT_TRUE (mgr.disable_user ("bob").has_value ());
  auto res = mgr.validate_user ("bob");
  ASSERT_FALSE (res.has_value ());
  // -32020 for disabled as well
}

// -----------------------------------------------------------------------
// 4. enable / disable
// -----------------------------------------------------------------------
TEST_F (UserManagerTest, toggle_enable)
{
  auto mgr = makeManager ();
  ASSERT_TRUE (mgr.register_user ("charlie").has_value ());
  ASSERT_TRUE (mgr.disable_user ("charlie").has_value ());
  auto res = mgr.validate_user ("charlie");
  ASSERT_FALSE (res.has_value ());

  ASSERT_TRUE (mgr.enable_user ("charlie").has_value ());
  auto res2 = mgr.validate_user ("charlie");
  ASSERT_TRUE (res2.has_value ());
}

// -----------------------------------------------------------------------
// 5. get_profile (empty user)
// -----------------------------------------------------------------------
TEST_F (UserManagerTest, profile_no_jobs)
{
  auto mgr = makeManager ();
  ASSERT_TRUE (mgr.register_user ("empty").has_value ());
  auto profile = mgr.get_profile ("empty");
  ASSERT_TRUE (profile.has_value ());
  EXPECT_EQ (profile->user_id, "empty");
  EXPECT_EQ (profile->total_jobs, 0);
  EXPECT_EQ (profile->successful_jobs, 0);
  EXPECT_EQ (profile->failed_jobs, 0);
  EXPECT_FALSE (profile->last_seen.has_value ());
  EXPECT_TRUE (profile->connected_providers.empty ());
}

// -----------------------------------------------------------------------
// 6. list_users pagination & filtering
// -----------------------------------------------------------------------
TEST_F (UserManagerTest, list_users_pagination)
{
  auto mgr = makeManager ();
  for (int i = 0; i < 5; ++i)
  {
    ASSERT_TRUE (mgr.register_user ("user_" + std::to_string (i)).has_value ());
  }
  // first page: limit 2 offset 0
  auto page1 = mgr.list_users (std::nullopt, 2, 0);
  ASSERT_TRUE (page1.has_value ());
  EXPECT_EQ (page1->size (), 2u);

  // second page: offset 2
  auto page2 = mgr.list_users (std::nullopt, 2, 2);
  ASSERT_TRUE (page2.has_value ());
  EXPECT_EQ (page2->size (), 2u);

  // last page: offset 4, 1 user remaining (5 total, offset 4 → 1 left)
  auto page3 = mgr.list_users (std::nullopt, 10, 4);
  ASSERT_TRUE (page3.has_value ());
  EXPECT_EQ (page3->size (), 1u);
}

TEST_F (UserManagerTest, list_users_filter_enabled)
{
  auto mgr = makeManager ();
  ASSERT_TRUE (mgr.register_user ("active").has_value ());
  ASSERT_TRUE (mgr.register_user ("inactive").has_value ());
  ASSERT_TRUE (mgr.disable_user ("inactive").has_value ());

  auto enabled = mgr.list_users (true, 100, 0);
  ASSERT_TRUE (enabled.has_value ());
  bool foundInactive = false;
  for (const auto &u : *enabled)
  {
    if (u.id == "inactive")
      foundInactive = true;
  }
  EXPECT_FALSE (foundInactive);
}

// -----------------------------------------------------------------------
// 7. count_users
// -----------------------------------------------------------------------
TEST_F (UserManagerTest, count_users)
{
  auto mgr = makeManager ();
  ASSERT_TRUE (mgr.register_user ("a").has_value ());
  ASSERT_TRUE (mgr.register_user ("b").has_value ());
  ASSERT_TRUE (mgr.register_user ("c").has_value ());
  ASSERT_TRUE (mgr.disable_user ("b").has_value ());

  auto cntAll = mgr.count_users (std::nullopt);
  ASSERT_TRUE (cntAll.has_value ());
  EXPECT_EQ (*cntAll, 3);

  auto cntEnabled = mgr.count_users (true);
  ASSERT_TRUE (cntEnabled.has_value ());
  EXPECT_EQ (*cntEnabled, 2);

  auto cntDisabled = mgr.count_users (false);
  ASSERT_TRUE (cntDisabled.has_value ());
  EXPECT_EQ (*cntDisabled, 1);
}
