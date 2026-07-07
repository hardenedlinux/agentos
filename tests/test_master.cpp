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

/**
 * test_master.cpp
 *
 * Black-box tests for Master (ADR-024 Actor model).
 * Master is started as a real Actor thread; on_message() always returns
 * immediately (LLM calls run in detached threads). Results are observed
 * via a mock send_to_orchestrator callback.
 *
 * Non-LLM coverage (MasterTest):
 *   - JobSubmit with no advisers registered → job_failed
 *   - JobSubmit with exactly one adviser → select_adviser skips LLM,
 *     directly forwards spawn_adviser
 *   - JobSubmit with missing goal → job_failed immediately (no thread)
 *   - AdviserFailed → job_failed
 *
 * LLM-dependent coverage (MasterLlmTest, skipped without DEEPSEEK_API_KEY):
 *   - JobSubmit with 2+ advisers → LLM selects one
 *   - WorkerExhausted → LLM forge decision → trigger_forge or job_failed
 */

#include <gtest/gtest.h>

#include "agentos/central.h" // for Config
#include "agentos/database.h"
#include "agentos/home_init.h"
#include "agentos/llm_client.h"
#include "agentos/llm_proxy.h"
#include "agentos/master.h"
#include "agentos/registry.h"
#include "agentos/types.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;
using namespace agentos;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class MasterTest : public ::testing::Test
{
protected:
  fs::path home_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<Registry> registry_;
  std::unique_ptr<LlmProxy> llm_proxy_;
  std::unique_ptr<LlmClient> llm_client_;
  Config config_;
  std::unique_ptr<Master> master_;

  std::mutex mtx_;
  std::condition_variable cv_;
  std::vector<OrchestratorEvent> orch_events_;

  void SetUp () override
  {
    char tmpl[] = "/tmp/agentos_master_test_XXXXXX";
    char *dir = mkdtemp (tmpl);
    ASSERT_NE (dir, nullptr);
    home_ = dir;
    setenv ("AGENTOS_HOME", home_.c_str (), 1);
    agentos::initialise_home (home_);

    db_ = std::make_unique<Database> ((home_ / "agentos.db").string ());
    ASSERT_TRUE (db_->open ());
  }

  // Construct registry/llm/master after any DB seeding is done by the test.
  void start_master ()
  {
    registry_ = std::make_unique<Registry> ();
    registry_->init (*db_);
    llm_proxy_ = std::make_unique<LlmProxy> (1, 5);
    llm_client_ = std::make_unique<LlmClient> (*llm_proxy_, config_.llm);

    master_
      = std::make_unique<Master> (*llm_client_, *registry_,
                                  [this] (OrchestratorEvent ev)
                                  {
                                    std::lock_guard<std::mutex> lk (mtx_);
                                    orch_events_.push_back (std::move (ev));
                                    cv_.notify_all ();
                                  });

    master_->start ();
  }

  void TearDown () override
  {
    if (master_)
      master_->stop ();
    db_->close ();
    unsetenv ("AGENTOS_HOME");
    fs::remove_all (home_);
  }

  bool wait_orch (size_t n, int timeout_ms = 5000)
  {
    std::unique_lock<std::mutex> lk (mtx_);
    return cv_.wait_for (lk, std::chrono::milliseconds (timeout_ms),
                         [&] { return orch_events_.size () >= n; });
  }
};

// ---------------------------------------------------------------------------
// JobSubmit, no advisers registered → job_failed
// ---------------------------------------------------------------------------

TEST_F (MasterTest, JobSubmit_NoAdvisers_JobFailed)
{
  start_master (); // empty DB → registry_.all_advisers() is empty

  MasterEvent ev;
  ev.kind = MasterEvent::Kind::JobSubmit;
  ev.job_id = "job-1";
  ev.payload_json = R"({"goal":"do something"})";
  master_->enqueue (std::move (ev));

  ASSERT_TRUE (wait_orch (1));
  std::lock_guard<std::mutex> lk (mtx_);
  ASSERT_EQ (orch_events_.size (), 1u);
  EXPECT_EQ (orch_events_[0].kind, OrchestratorEvent::Kind::MasterDecision);
  EXPECT_NE (orch_events_[0].payload_json.find ("job_failed"),
             std::string::npos);
  EXPECT_NE (orch_events_[0].payload_json.find ("job-1"), std::string::npos);
}

// ---------------------------------------------------------------------------
// JobSubmit, missing goal → job_failed immediately, no detached thread
// ---------------------------------------------------------------------------

TEST_F (MasterTest, JobSubmit_MissingGoal_JobFailedImmediately)
{
  start_master ();

  MasterEvent ev;
  ev.kind = MasterEvent::Kind::JobSubmit;
  ev.job_id = "job-2";
  ev.payload_json = R"({})"; // no "goal" field
  master_->enqueue (std::move (ev));

  ASSERT_TRUE (wait_orch (1));
  std::lock_guard<std::mutex> lk (mtx_);
  ASSERT_EQ (orch_events_.size (), 1u);
  EXPECT_NE (orch_events_[0].payload_json.find ("job_failed"),
             std::string::npos);
  EXPECT_NE (orch_events_[0].payload_json.find ("missing goal"),
             std::string::npos);
}

// ---------------------------------------------------------------------------
// JobSubmit, exactly one adviser → select_adviser skips LLM, spawn_adviser
// ---------------------------------------------------------------------------

TEST_F (MasterTest, JobSubmit_SingleAdviser_SkipsLlmAndSpawns)
{
  // Insert one adviser agent row before constructing Registry.
  db_->insert_agent ("planning-adviser", "adviser", "", R"({
    "id": "planning-adviser",
    "name": "Planning Adviser",
    "version": "1.0.0",
    "skill_path": "advisers/planning/skill.md",
    "domains": ["planning"]
  })");

  start_master ();

  ASSERT_EQ (registry_->all_advisers ().size (), 1u)
    << "test setup: expected exactly one adviser to be loaded from DB";

  MasterEvent ev;
  ev.kind = MasterEvent::Kind::JobSubmit;
  ev.job_id = "job-3";
  // Goal must contain a token that exactly matches the adviser's "planning"
  // domain tag (ADR-033 §1: token-overlap match, no stemming). "plan
  // something" tokenizes to ["plan","something"] and would never match
  // "planning" — it must be the exact word, not a prefix/stem of it.
  ev.payload_json = R"({"goal":"outline a planning workflow"})";
  master_->enqueue (std::move (ev));

  ASSERT_TRUE (wait_orch (1));
  std::lock_guard<std::mutex> lk (mtx_);
  ASSERT_EQ (orch_events_.size (), 1u);
  EXPECT_NE (orch_events_[0].payload_json.find ("spawn_adviser"),
             std::string::npos);
  EXPECT_NE (orch_events_[0].payload_json.find ("planning-adviser"),
             std::string::npos);
  EXPECT_NE (orch_events_[0].payload_json.find ("job-3"), std::string::npos);
}

// ---------------------------------------------------------------------------
// AdviserFailed → job_failed
// ---------------------------------------------------------------------------

TEST_F (MasterTest, AdviserFailed_JobFailed)
{
  start_master ();

  MasterEvent ev;
  ev.kind = MasterEvent::Kind::AdviserFailed;
  ev.job_id = "job-4";
  master_->enqueue (std::move (ev));

  ASSERT_TRUE (wait_orch (1));
  std::lock_guard<std::mutex> lk (mtx_);
  ASSERT_EQ (orch_events_.size (), 1u);
  EXPECT_NE (orch_events_[0].payload_json.find ("job_failed"),
             std::string::npos);
  EXPECT_NE (orch_events_[0].payload_json.find ("job-4"), std::string::npos);
}

// ---------------------------------------------------------------------------
// on_message never blocks — multiple JobSubmits processed promptly even
// while detached LLM threads are (would be) in flight.
// ---------------------------------------------------------------------------

TEST_F (MasterTest, OnMessage_DoesNotBlock_ProcessesMultipleQuickly)
{
  start_master (); // 0 advisers → each JobSubmit fails fast, no LLM thread

  for (int i = 0; i < 5; ++i)
  {
    MasterEvent ev;
    ev.kind = MasterEvent::Kind::JobSubmit;
    ev.job_id = "job-bulk-" + std::to_string (i);
    ev.payload_json = R"({"goal":"x"})";
    master_->enqueue (std::move (ev));
  }

  ASSERT_TRUE (wait_orch (5, 2000));
  std::lock_guard<std::mutex> lk (mtx_);
  EXPECT_EQ (orch_events_.size (), 5u);
}

// ---------------------------------------------------------------------------
// LLM-dependent tests
// ---------------------------------------------------------------------------

class MasterLlmTest : public MasterTest
{
protected:
  void SetUp () override
  {
    MasterTest::SetUp ();

    const char *key = std::getenv ("DEEPSEEK_API_KEY");
    if (!key || std::strlen (key) == 0)
      GTEST_SKIP () << "DEEPSEEK_API_KEY not set — skipping LLM tests";

    setenv ("AGENTOS_ADVISER_API_KEY", key, 1);
    config_.llm.base_url = "https://api.deepseek.com";
    config_.llm.model = "deepseek-chat";
    config_.llm.api_key = key;
  }

  void TearDown () override
  {
    unsetenv ("AGENTOS_ADVISER_API_KEY");
    MasterTest::TearDown ();
  }
};

// Two advisers registered → LLM must pick one of the two valid ids.
TEST_F (MasterLlmTest, JobSubmit_TwoAdvisers_LlmSelectsOne)
{
  // Both advisers share the "code" domain tag so the goal below produces
  // exactly two candidates (ADR-033 §1 Step 1), forcing selection into
  // Step 2's bounded LLM disambiguation rather than short-circuiting on a
  // single candidate or falling back to zero-candidate 'planning'.
  db_->insert_agent ("planning-adviser", "adviser", "", R"({
    "id": "planning-adviser",
    "name": "Planning Adviser",
    "version": "1.0.0",
    "skill_path": "advisers/planning/skill.md",
    "domains": ["planning", "general", "code"]
  })");
  db_->insert_agent ("code-writer", "adviser", "", R"({
    "id": "code-writer",
    "name": "Code Writer",
    "version": "1.0.0",
    "skill_path": "advisers/code-writer/skill.md",
    "domains": ["code"]
  })");

  start_master ();
  ASSERT_EQ (registry_->all_advisers ().size (), 2u);

  MasterEvent ev;
  ev.kind = MasterEvent::Kind::JobSubmit;
  ev.job_id = "job-llm-1";
  // Goal must contain the exact word "code" (both advisers' shared domain
  // tag) to actually reach 2 candidates — "python function to sort a list"
  // alone tokenizes with no token matching "code", "planning", or
  // "general", and would silently fall through to the zero-candidate path.
  ev.payload_json
    = R"({"goal":"write code to sort a list in python"})";
  master_->enqueue (std::move (ev));

  ASSERT_TRUE (wait_orch (1, 60000));
  std::lock_guard<std::mutex> lk (mtx_);
  ASSERT_EQ (orch_events_.size (), 1u);
  EXPECT_NE (orch_events_[0].payload_json.find ("spawn_adviser"),
             std::string::npos);
  const bool picked_one_of_two
    = orch_events_[0].payload_json.find ("planning-adviser")
        != std::string::npos
      || orch_events_[0].payload_json.find ("code-writer") != std::string::npos;
  EXPECT_TRUE (picked_one_of_two);
}

// WorkerExhausted → LLM forge decision → either trigger_forge or job_failed,
// never a hard crash / empty payload.
TEST_F (MasterLlmTest, WorkerExhausted_ProducesForgeDecisionOrFailure)
{
  start_master ();

  MasterEvent ev;
  ev.kind = MasterEvent::Kind::WorkerExhausted;
  ev.job_id = "job-llm-2";
  ev.payload_json = R"({"command":"video.transcode"})";
  master_->enqueue (std::move (ev));

  ASSERT_TRUE (wait_orch (1, 60000));
  std::lock_guard<std::mutex> lk (mtx_);
  ASSERT_EQ (orch_events_.size (), 1u);
  const std::string &payload = orch_events_[0].payload_json;
  const bool valid = payload.find ("trigger_forge") != std::string::npos
                     || payload.find ("job_failed") != std::string::npos;
  EXPECT_TRUE (valid) << "unexpected payload: " << payload;
}
