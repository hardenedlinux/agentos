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
 * test_orchestrator.cpp
 *
 * Black-box tests for Orchestrator (ADR-022).
 * Orchestrator is started as a real Actor thread; messages are enqueued
 * and outcomes are observed via mock send_to_master / send_to_gateway
 * callbacks (condition_variable-synchronised).
 *
 * Covers:
 *   - Authentication: missing key, invalid key, valid key + role permission
 *   - job.submit: persists job, replies with job_id, forwards JobSubmit to
 * Master
 *   - WorkerExhausted: no Worker registered for command → MasterEvent to Master
 *   - Pipeline dispatch: plan_ready with a registered worker → WorkerDone →
 * job done
 */

#include <gtest/gtest.h>

#include "agentos/central.h" // for Config (avoids guessing config.h path)
#include "agentos/cred_vault.h"
#include "agentos/database.h"
#include "agentos/dispatcher.h"
#include "agentos/forge_coordinator.h"
#include "agentos/home_init.h"
#include "agentos/llm_proxy.h"
#include "agentos/orchestrator.h"
#include "agentos/registry.h"
#include "agentos/types.h"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <vector>

namespace fs = std::filesystem;
using namespace agentos;

namespace
{

  // Mirrors Orchestrator's internal sha256_hex() — SHA-256(key || salt) hex.
  std::string sha256_hex (const std::string &key, const std::string &salt)
  {
    const std::string data = key + salt;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    EVP_MD_CTX *ctx = EVP_MD_CTX_new ();
    EVP_DigestInit_ex (ctx, EVP_sha256 (), nullptr);
    EVP_DigestUpdate (ctx, data.data (), data.size ());
    EVP_DigestFinal_ex (ctx, hash, nullptr);
    EVP_MD_CTX_free (ctx);

    char hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
      snprintf (hex + i * 2, 3, "%02x", hash[i]);
    return std::string (hex, SHA256_DIGEST_LENGTH * 2);
  }

} // anonymous namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class OrchestratorTest : public ::testing::Test
{
protected:
  fs::path home_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<Registry> registry_;
  std::unique_ptr<LlmProxy> llm_;
  Dispatcher dispatcher_;
  std::unique_ptr<forge::ForgeCoordinator> forge_;
  Config config_;
  std::unique_ptr<CredVault> cred_vault_;
  std::unique_ptr<Orchestrator> orch_;

  // Captured outputs from mock callbacks.
  std::mutex mtx_;
  std::condition_variable cv_;
  std::vector<MasterEvent> master_events_;
  std::vector<GatewayEvent> gateway_events_;

  void SetUp () override
  {
    char tmpl[] = "/tmp/agentos_orch_test_XXXXXX";
    char *dir = mkdtemp (tmpl);
    ASSERT_NE (dir, nullptr);
    home_ = dir;
    setenv ("AGENTOS_HOME", home_.c_str (), 1);
    agentos::initialise_home (home_);

    db_ = std::make_unique<Database> ((home_ / "agentos.db").string ());
    ASSERT_TRUE (db_->open ());

    registry_ = std::make_unique<Registry> ();
    registry_->init (*db_);
    llm_ = std::make_unique<LlmProxy> (1, 5);

    forge_ = std::make_unique<forge::ForgeCoordinator> (
      *db_, *llm_, *registry_,
      [] (forge::ForgeResult) { /* unused in these tests */ });

    Config::Vault vault_cfg{};
    cred_vault_ = std::make_unique<CredVault> (*db_, vault_cfg);
    auto r = cred_vault_->start ();
    ASSERT_TRUE (r.has_value ()) << r.error ();

    orch_ = std::make_unique<Orchestrator> (
      *db_, *llm_, *registry_, dispatcher_, *forge_, config_, *cred_vault_,
      [this] (MasterEvent ev)
      {
        std::lock_guard<std::mutex> lk (mtx_);
        master_events_.push_back (std::move (ev));
        cv_.notify_all ();
      },
      [this] (GatewayEvent ev)
      {
        std::lock_guard<std::mutex> lk (mtx_);
        gateway_events_.push_back (std::move (ev));
        cv_.notify_all ();
      });

    orch_->init ();
    orch_->start ();
  }

  void TearDown () override
  {
    orch_->stop ();
    forge_->stop ();
    if (cred_vault_)
      cred_vault_->stop ();
    db_->close ();
    unsetenv ("AGENTOS_HOME");
    fs::remove_all (home_);
  }

  // Insert an access key directly into DB (bypassing CLI key generation).
  // Returns the plaintext key value.
  //
  // Orchestrator::authenticate() rejects any key that isn't exactly 64
  // lowercase-hex characters (production keys are hex-encoded 32-byte
  // random values) *before* it ever looks the key up in active_keys_ or
  // compares hashes. A prior version of this helper generated
  // "plain-<role>-key", which fails that format check outright — every
  // job.submit-related test using it got -32010 "Failed to authorize"
  // regardless of role or params, never reaching the code paths those
  // tests actually intended to exercise. This version deterministically
  // maps the role name into a valid 64-char hex string so different roles
  // still get distinguishable keys.
  std::string insert_key (const std::string &role)
  {
    Database::AccessKey ak;
    ak.id = "key-" + role;
    std::string hex_from_role;
    for (unsigned char c : role)
      hex_from_role += "0123456789abcdef"[c % 16];
    ak.key = hex_from_role + std::string (64 - hex_from_role.size (), '0');
    ak.key_salt = "salt-" + role;
    ak.key_hash = sha256_hex (ak.key, ak.key_salt);
    ak.description = role + " key";
    ak.role = role;
    ak.created_at = 1000;
    db_->insert_access_key (ak);
    return ak.key;
  }

  // Send a raw GatewayInbound to Orchestrator.
  void send_inbound (const std::string &payload_json)
  {
    OrchestratorEvent ev;
    ev.kind = OrchestratorEvent::Kind::GatewayInbound;
    ev.payload_json = payload_json;
    orch_->enqueue (std::move (ev));
  }

  // Wait until at least `n` gateway events have been captured.
  bool wait_gateway (size_t n, int timeout_ms = 2000)
  {
    std::unique_lock<std::mutex> lk (mtx_);
    return cv_.wait_for (lk, std::chrono::milliseconds (timeout_ms),
                         [&] { return gateway_events_.size () >= n; });
  }

  bool wait_master (size_t n, int timeout_ms = 2000)
  {
    std::unique_lock<std::mutex> lk (mtx_);
    return cv_.wait_for (lk, std::chrono::milliseconds (timeout_ms),
                         [&] { return master_events_.size () >= n; });
  }
};

// ---------------------------------------------------------------------------
// Authentication: missing key
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, MissingKey_Unauthorized)
{
  send_inbound (R"({"jsonrpc":"2.0","id":"1","method":"job.submit",)"
                R"("key":"","params":{"goal":"do something"}})");

  ASSERT_TRUE (wait_gateway (1));
  std::lock_guard<std::mutex> lk (mtx_);
  ASSERT_EQ (gateway_events_.size (), 1u);
  EXPECT_NE (gateway_events_[0].outbound.message.find ("-32010"),
             std::string::npos);
}

// ---------------------------------------------------------------------------
// Authentication: invalid key (not in DB)
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, InvalidKey_Unauthorized)
{
  send_inbound (R"({"jsonrpc":"2.0","id":"1","method":"job.submit",)"
                R"("key":"nonexistent-key","params":{"goal":"x"}})");

  ASSERT_TRUE (wait_gateway (1));
  std::lock_guard<std::mutex> lk (mtx_);
  EXPECT_NE (gateway_events_[0].outbound.message.find ("-32010"),
             std::string::npos);
}

// ---------------------------------------------------------------------------
// Authentication: readonly key calling job.submit → Forbidden
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, ReadonlyKey_JobSubmit_Forbidden)
{
  const std::string key = insert_key ("readonly");
  orch_->stop ();
  orch_->init (); // reload active_keys_ cache after DB insert
  orch_->start ();

  send_inbound (R"({"jsonrpc":"2.0","id":"1","method":"job.submit",)"
                R"("key":")"
                + key + R"(","params":{"goal":"x"}})");

  ASSERT_TRUE (wait_gateway (1));
  std::lock_guard<std::mutex> lk (mtx_);
  EXPECT_NE (gateway_events_[0].outbound.message.find ("-32011"),
             std::string::npos)
    << "actual message: " << gateway_events_[0].outbound.message;
}

TEST_F (OrchestratorTest, ReadonlyKey_JobStatus_Permitted)
{
  const std::string key = insert_key ("readonly");
  orch_->stop ();
  orch_->init ();
  orch_->start ();

  send_inbound (R"({"jsonrpc":"2.0","id":"1","method":"job.status",)"
                R"("key":")"
                + key + R"(","params":{"job_id":"nope"}})");

  ASSERT_TRUE (wait_gateway (1));
  std::lock_guard<std::mutex> lk (mtx_);
  // Not -32011 (forbidden) — should be -32020 (not found) since job doesn't
  // exist.
  EXPECT_EQ (gateway_events_[0].outbound.message.find ("-32011"),
             std::string::npos);
}

// ---------------------------------------------------------------------------
// job.submit: valid operator key → reply with job_id, forward to Master
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, JobSubmit_ValidKey_RepliesAndForwardsToMaster)
{
  const std::string key = insert_key ("operator");
  orch_->stop ();
  orch_->init ();
  orch_->start ();

  send_inbound (R"({"jsonrpc":"2.0","id":"req-1","method":"job.submit",)"
                R"("key":")"
                + key + R"(","params":{"goal":"summarise a document"}})");

  ASSERT_TRUE (wait_gateway (1));

  {
    std::lock_guard<std::mutex> lk (mtx_);
    std::cerr << "[diag] gateway reply after job.submit: "
              << gateway_events_[0].outbound.message << std::endl;
    std::cerr << "[diag] master_events_.size() at this point: "
              << master_events_.size () << std::endl;
  }

  ASSERT_TRUE (wait_master (1))
    << "Master never received JobSubmit — see [diag] gateway reply above "
       "for what job.submit actually replied with.";

  {
    std::lock_guard<std::mutex> lk (mtx_);
    // Gateway reply contains a job_id.
    EXPECT_NE (gateway_events_[0].outbound.message.find ("\"job_id\""),
               std::string::npos);
    EXPECT_NE (gateway_events_[0].outbound.message.find ("\"req-1\""),
               std::string::npos);

    // Master received JobSubmit with the goal.
    ASSERT_EQ (master_events_.size (), 1u);
    EXPECT_EQ (master_events_[0].kind, MasterEvent::Kind::JobSubmit);
    EXPECT_NE (master_events_[0].payload_json.find ("summarise a document"),
               std::string::npos);
    EXPECT_FALSE (master_events_[0].job_id.empty ());
  }
}

// ---------------------------------------------------------------------------
// job.submit: missing goal → Invalid params
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, JobSubmit_MissingGoal_InvalidParams)
{
  const std::string key = insert_key ("operator");
  orch_->stop ();
  orch_->init ();
  orch_->start ();

  send_inbound (R"({"jsonrpc":"2.0","id":"1","method":"job.submit",)"
                R"("key":")"
                + key + R"(","params":{}})");

  ASSERT_TRUE (wait_gateway (1));
  std::lock_guard<std::mutex> lk (mtx_);
  EXPECT_NE (gateway_events_[0].outbound.message.find ("-32602"),
             std::string::npos)
    << "actual message: " << gateway_events_[0].outbound.message;
}

// ---------------------------------------------------------------------------
// MasterDecision plan_ready with no registered worker → WorkerExhausted to
// Master
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, PlanReady_NoWorkerForCommand_ReportsExhaustedToMaster)
{
  const std::string job_id = "job-exhaust-1";

  // First create the job row (job.submit normally does this).
  Task task;
  task.id = TaskId (job_id);
  task.goal = "do the thing";
  db_->store_job (task);

  OrchestratorEvent ev;
  ev.kind = OrchestratorEvent::Kind::MasterDecision;
  ev.job_id = job_id;
  ev.payload_json
    = R"({"type":"plan_ready","job_id":")" + job_id
      + R"(","job_type":"oneshot","steps":[)"
        R"({"id":"step-1","command":"nonexistent.command","description":"d"})"
        R"(]})";
  orch_->enqueue (std::move (ev));

  ASSERT_TRUE (wait_master (1));
  std::lock_guard<std::mutex> lk (mtx_);
  ASSERT_EQ (master_events_.size (), 1u);
  EXPECT_EQ (master_events_[0].kind, MasterEvent::Kind::WorkerExhausted);
  EXPECT_EQ (master_events_[0].job_id, job_id);
  EXPECT_NE (master_events_[0].payload_json.find ("nonexistent.command"),
             std::string::npos);
}

// ---------------------------------------------------------------------------
// Pipeline: plan_ready with a registered worker → WorkerDone → job done
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, PlanReady_RegisteredWorker_RunsAndCompletes)
{
  // Write a minimal worker script conforming to the Result File Wire Format
  // (ADR-016): it must write {"status":"ok","result":...} to
  // $AGENTOS_RUN_DIR/result.json before exiting. Placed under
  // ~/.agentos/workers/ so it is covered by the implicit Landlock READ_FILE
  // grant in sandbox.cpp's apply_worker_sandbox().
  const fs::path worker_dir = home_ / "workers" / "worker-true";
  fs::create_directories (worker_dir);
  const fs::path worker_script = worker_dir / "worker.sh";
  {
    std::ofstream f (worker_script);
    ASSERT_TRUE (f.is_open ());
    f << "#!/bin/sh\n"
         "echo '{\"status\":\"ok\",\"result\":{}}' "
         "> \"$AGENTOS_RUN_DIR/result.json\"\n";
  }
  fs::permissions (worker_script,
                   fs::perms::owner_all | fs::perms::group_read
                     | fs::perms::group_exec | fs::perms::others_read
                     | fs::perms::others_exec);

  // Register a worker directly via DB (Registry loads at construction,
  // so we must insert before constructing — re-create registry+orchestrator).
  db_->insert_agent ("worker-true", "worker", worker_script.string (), "{}");
  db_->insert_capability ("worker-true", "echo.test", "test capability", "{}");

  // Rebuild registry and orchestrator so the new agent is loaded.
  orch_->stop ();
  registry_ = std::make_unique<Registry> ();
  registry_->init (*db_);
  orch_ = std::make_unique<Orchestrator> (
    *db_, *llm_, *registry_, dispatcher_, *forge_, config_, *cred_vault_,
    [this] (MasterEvent ev)
    {
      std::lock_guard<std::mutex> lk (mtx_);
      master_events_.push_back (std::move (ev));
      cv_.notify_all ();
    },
    [this] (GatewayEvent ev)
    {
      std::lock_guard<std::mutex> lk (mtx_);
      gateway_events_.push_back (std::move (ev));
      cv_.notify_all ();
    });
  orch_->init ();
  orch_->start ();

  const std::string job_id = "job-run-true";
  Task task;
  task.id = TaskId (job_id);
  task.goal = "run true";
  db_->store_job (task);

  OrchestratorEvent ev;
  ev.kind = OrchestratorEvent::Kind::MasterDecision;
  ev.job_id = job_id;
  ev.payload_json
    = R"({"type":"plan_ready","job_id":")" + job_id
      + R"(","job_type":"oneshot","steps":[)"
        R"({"id":"step-1","command":"echo.test","description":"d"})"
        R"(]})";
  orch_->enqueue (std::move (ev));

  // Worker exits quickly; reaper must be invoked manually in tests
  // (PeriodicExecutor is not running here) — poll dispatcher_.reap().
  bool done = false;
  for (int i = 0; i < 100 && !done; ++i)
  {
    dispatcher_.reap ();
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    std::lock_guard<std::mutex> lk (mtx_);
    for (const auto &gev : gateway_events_)
      if (gev.outbound.message.find ("job.phase_changed") != std::string::npos
          && gev.outbound.message.find ("\"done\"") != std::string::npos)
        done = true;
  }

  EXPECT_TRUE (done) << "job did not reach 'done' phase";
}

