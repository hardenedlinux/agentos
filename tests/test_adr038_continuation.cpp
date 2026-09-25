/**
 * ADR-038 interaction continuation tests.
 *
 * Scope note (important — read before adding to this file): none of these
 * tests exercise Orchestrator's dispatch logic (the registry-first fast
 * path check, prompt injection, or the post-completion write triggered by
 * an Adviser's actual LLM response). Constructing a real Orchestrator in
 * this test binary would require a live Dispatcher and ForgeCoordinator,
 * which this test fixture does not build. What's covered here is the
 * Database layer's transactional read-and-consume semantics and the
 * Registry's in-memory supports_continuation flag — both are real
 * dependencies of the Orchestrator-side logic and are worth testing in
 * isolation, but a regression introduced purely in orchestrator.cpp's
 * dispatch code (not in Database or Registry) would NOT be caught by any
 * test in this file. See ADR-038 review notes for the specific known gaps
 * this leaves (entry-adviser two-hop dispatch, bridge_hint delivery on a
 * real job completion, pending_continuation_ids_ cleanup) — those require
 * a real Orchestrator + mock LLM + Dispatcher and are tracked separately,
 * not silently declared "done" by anything in this file.
 *
 * Uses Google Test (existing project convention).
 */

#include "agentos/database.h"
#include "agentos/home_init.h"
#include "agentos/registry.h"
#include "agentos/time_utils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

namespace fs = std::filesystem;

using namespace agentos;

// -----------------------------------------------------------------------
// Test fixture: a temporary AgentOS home + Database + Registry.
// -----------------------------------------------------------------------
class Adr038ContinuationTest : public ::testing::Test
{
protected:
  void SetUp () override
  {
    // Create a fresh temp directory for agentos_home().
    char tmpl[] = "/tmp/agentos_adr038_XXXXXX";
    char *dir = mkdtemp (tmpl);
    ASSERT_NE (dir, nullptr);
    home_dir_ = dir;
    setenv ("AGENTOS_HOME", home_dir_.c_str (), 1);

    // Ensure the directory layout that initialise_home would normally write.
    initialise_home (home_dir_);

    db_path_ = home_dir_ / "agentos.db";
    db_ = std::make_unique<Database> (db_path_.string ());
    ASSERT_TRUE (db_->open ());

    registry_ = std::make_unique<Registry> ();
    registry_->init (*db_);
  }

  void TearDown () override
  {
    db_.reset ();
    registry_.reset ();
    unsetenv ("AGENTOS_HOME");
    if (!home_dir_.empty ())
      fs::remove_all (home_dir_);
  }

  // Register an adviser directly against the DB — this reproduces the
  // effect Orchestrator::register_adviser_package has on the `agents`
  // table (insert_agent + set_agent_supports_continuation), without
  // requiring a live Orchestrator instance. Registry::init() builds its
  // in-memory RegisteredAdviser list from AgentRow.supports_continuation
  // (a DB column, populated below), not by re-parsing manifest.toml off
  // disk — so no manifest file is needed for these tests to exercise the
  // Registry's opt-in flag correctly.
  //
  // Sets adviser_id_ to `id` and refreshes registry_ so
  // registry_->find_adviser_by_id(id) reflects the new row immediately.
  void install_adviser (const std::string &id, bool supports_continuation,
                       const std::string &description = "test adviser")
  {
    const fs::path dir = home_dir_ / "advisers" / id;
    fs::create_directories (dir);
    const std::string skill_path = (dir / "skill.md").string ();
    std::ofstream sk (skill_path);
    ASSERT_TRUE (sk.is_open ());
    sk << "dummy";
    sk.close ();

    db_->insert_agent (id, "adviser", skill_path, /*manifest*/ "",
                       description, "operator");
    if (supports_continuation)
      db_->set_agent_supports_continuation (id, true);

    adviser_id_ = id;
    registry_->init (*db_);
  }

  // Insert a continuation row directly for setup.
  void insert_continuation (const std::string &cid, const std::string &user,
                            const std::string &adviser,
                            const std::string &payload)
  {
    Database::InteractionContinuationRow row;
    row.continuation_id = cid;
    row.user_id = user;
    row.adviser_id = adviser;
    row.context_payload = payload;
    row.created_at = now_unix ();
    row.consumed_at = std::nullopt;
    db_->insert_interaction_continuation (row);
  }

  // -----------------------------------------------------------------------
  //  Members
  // -----------------------------------------------------------------------
  fs::path home_dir_;
  fs::path db_path_;
  std::string adviser_id_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<Registry> registry_;
};

// -----------------------------------------------------------------------
// Test 1 — two-hop chain via the Database layer's public API only.
//
// This does NOT exercise Orchestrator dispatch (no prompt injection, no
// registry-gated skip, no bridge_hint file/field). It only confirms that
// the write-then-consume-then-write-again chain the Orchestrator's
// dispatch code is built on top of behaves correctly at the Database
// layer: a row written by a simulated "turn 1" post-completion is
// consumable exactly once, and a simulated "turn 2" post-completion
// produces an independent new row. bridge_hint delivery on a real job
// completion is out of scope here — it requires a live Orchestrator and
// is tracked separately (see file header).
// -----------------------------------------------------------------------
TEST_F (Adr038ContinuationTest, TwoHopChainAtDatabaseLayer)
{
  install_adviser ("test-cont", /*supports_continuation=*/true);

  const std::string user_id = "0";

  // Simulated turn-1 post-completion write.
  const std::string cid1 = "cont-0001";
  const std::string payload1 = "{\"phase\":\"turn1\"}";
  insert_continuation (cid1, user_id, adviser_id_, payload1);

  // Simulated turn-2 dispatch-time consume.
  auto consumed = db_->read_and_consume_continuation (cid1, user_id,
                                                       adviser_id_);
  ASSERT_TRUE (consumed.has_value ());
  EXPECT_EQ (consumed->context_payload, payload1);

  // Replay protection: the same id must not be consumable twice. This is
  // also the only public-API way to confirm consumed_at was actually set
  // (Database::prepare() is private; there is no public "peek" accessor).
  {
    auto replay = db_->read_and_consume_continuation (cid1, user_id,
                                                       adviser_id_);
    EXPECT_FALSE (replay.has_value ());
  }

  // Simulated turn-2 post-completion write (a fresh, independent row).
  const std::string cid2 = "cont-0002";
  const std::string payload2 = "{\"hello\":\"world\"}";
  insert_continuation (cid2, user_id, adviser_id_, payload2);

  auto row2 = db_->read_and_consume_continuation (cid2, user_id, adviser_id_);
  ASSERT_TRUE (row2.has_value ());
  EXPECT_EQ (row2->context_payload, payload2);
  EXPECT_EQ (row2->user_id, user_id);
  EXPECT_EQ (row2->adviser_id, adviser_id_);
}

// -----------------------------------------------------------------------
// Test 2 — invalid/expired/mismatched degradation (replay protection,
//          wrong user, wrong adviser, unknown id)
// -----------------------------------------------------------------------
TEST_F (Adr038ContinuationTest, ReplayProtectionAndMismatchDegradeSilently)
{
  install_adviser ("test-cont", /*supports_continuation=*/true);

  const std::string user_a = "0";
  const std::string adviser_a = adviser_id_;

  // (a) unknown id
  {
    auto r = db_->read_and_consume_continuation ("nonexistent", user_a,
                                                 adviser_a);
    EXPECT_FALSE (r.has_value ());
  }

  // (b) already consumed → replay protection
  const std::string cid = "replay-1";
  insert_continuation (cid, user_a, adviser_a, "payload-x");

  auto first = db_->read_and_consume_continuation (cid, user_a, adviser_a);
  ASSERT_TRUE (first.has_value ());
  EXPECT_EQ (first->context_payload, "payload-x");

  {
    auto second = db_->read_and_consume_continuation (cid, user_a, adviser_a);
    EXPECT_FALSE (second.has_value ());
  }

  // (c) different user — must not be consumable, and must remain
  // consumable afterward by its actual owner (proves the failed attempt
  // above didn't mark it consumed as a side effect).
  const std::string cid_user = "cid-diff-user";
  insert_continuation (cid_user, "other-user", adviser_a, "should-not-match");
  {
    auto r = db_->read_and_consume_continuation (cid_user, user_a, adviser_a);
    EXPECT_FALSE (r.has_value ());
  }
  {
    auto r = db_->read_and_consume_continuation (cid_user, "other-user",
                                                 adviser_a);
    ASSERT_TRUE (r.has_value ());
  }

  // (d) different adviser — a continuation authored for adviser A must not
  // be consumable via a request targeting adviser B, even with matching
  // continuation_id and user_id.
  install_adviser ("test-cont-2", /*supports_continuation=*/true);
  const std::string adviser_b = "test-cont-2";

  const std::string cid_adv = "cid-diff-adviser";
  insert_continuation (cid_adv, user_a, adviser_b, "payload-b");
  {
    auto r = db_->read_and_consume_continuation (cid_adv, user_a, adviser_a);
    EXPECT_FALSE (r.has_value ());
  }
  {
    auto r = db_->read_and_consume_continuation (cid_adv, user_a, adviser_b);
    ASSERT_TRUE (r.has_value ());
  }
}

// -----------------------------------------------------------------------
// Test 3 — Registry correctly reports supports_continuation = false for a
// non-opted-in adviser.
//
// This proves the data Orchestrator's dispatch code reads (the in-memory
// registry flag) is correct. It does NOT prove Orchestrator actually
// branches on that flag and skips the DB lookup for such an adviser — that
// requires exercising the real dispatch path in orchestrator.cpp, which
// this test file cannot do (see file header). Do not read this test as
// covering that behavior.
// -----------------------------------------------------------------------
TEST_F (Adr038ContinuationTest, NonOptInAdviserRegistryFlagIsFalse)
{
  install_adviser ("no-cont", /*supports_continuation=*/false);

  auto adv = registry_->find_adviser_by_id (adviser_id_);
  ASSERT_TRUE (adv.has_value ());
  EXPECT_FALSE (adv->supports_continuation);

  // A continuation row targeting this adviser is still fully functional at
  // the Database layer (the opt-in gate lives in Orchestrator, not in
  // Database) — inserted here only to confirm insert/consume still work
  // for an adviser row with supports_continuation = false; this is not a
  // claim about what Orchestrator would do with it.
  const std::string user_id = "0";
  const std::string cid = "cid-non-opt";
  insert_continuation (cid, user_id, adviser_id_, "will-be-ignored");
  auto r = db_->read_and_consume_continuation (cid, user_id, adviser_id_);
  ASSERT_TRUE (r.has_value ());
  EXPECT_EQ (r->context_payload, "will-be-ignored");
}

// -----------------------------------------------------------------------
// Test 4 — a freshly written row (as Orchestrator's post-completion path
// would write on turn 1, i.e. with no incoming continuation_id) round-trips
// correctly through insert + read-and-consume.
//
// Scope note: this confirms Database-layer round-trip fidelity given a
// correct user_id. It does NOT exercise the Orchestrator-side logic that
// resolves job_user_id via db_.load_job(job_id) on the no-incoming-
// continuation-id path — the specific code path a prior code review fixed
// a bug in. If that Orchestrator-side bug were reintroduced, this test
// would still pass, because the user_id here is supplied directly rather
// than resolved from a job the way Orchestrator does it. A regression test
// for that specific bug needs a real Orchestrator dispatch, tracked
// separately.
// -----------------------------------------------------------------------
TEST_F (Adr038ContinuationTest, FreshRowRoundTripsWithoutIncomingContinuationId)
{
  install_adviser ("test-cont", /*supports_continuation=*/true);

  const std::string user_id = "0";
  const std::string new_payload = "{\"first\":\"response\"}";
  insert_continuation ("fresh-1", user_id, adviser_id_, new_payload);

  auto r = db_->read_and_consume_continuation ("fresh-1", user_id,
                                               adviser_id_);
  ASSERT_TRUE (r.has_value ());
  EXPECT_EQ (r->context_payload, new_payload);
  EXPECT_EQ (r->user_id, user_id);
  EXPECT_EQ (r->adviser_id, adviser_id_);
}

// -----------------------------------------------------------------------
// Test 5 — pending_continuation_ids_ cleanup for the non-opt-in case.
//
// Not testable at this layer: pending_continuation_ids_ is a private,
// in-memory member of Orchestrator, and this test file builds no
// Orchestrator instance. Marked as skipped (not a fake pass) so test
// reports don't imply this is covered. Remove this skip only once a real
// Orchestrator-backed test replaces it.
// -----------------------------------------------------------------------
TEST_F (Adr038ContinuationTest, PendingContinuationIdsCleanupForNonOptIn)
{
  GTEST_SKIP () << "Requires a live Orchestrator instance to exercise "
                  "handle_master_decision's non-opt-in branch and observe "
                  "pending_continuation_ids_ — not testable at the "
                  "Database/Registry layer this test file is restricted "
                  "to. See ADR-038 review notes.";
}
