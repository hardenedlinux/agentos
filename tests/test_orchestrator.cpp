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
#include <sqlite3.h>

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
#include "agentos/user_manager.h"

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
// ADR-029 ownership: job.status requires user_id; a foreign job reads as not
// found; the default user "0" is a real owner.
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, JobStatus_Ownership)
{
  const std::string key = insert_key ("operator");
  orch_->stop ();
  orch_->init ();
  orch_->start ();

  Task alice_job;
  alice_job.id = TaskId ("job-alice");
  alice_job.goal = "g";
  alice_job.user_id = "alice";
  db_->store_job (alice_job);

  Task zero_job;
  zero_job.id = TaskId ("job-zero");
  zero_job.goal = "g";
  zero_job.user_id = "0";
  db_->store_job (zero_job);

  auto status = [&] (const std::string &id, const std::string &params)
  {
    send_inbound (R"({"jsonrpc":"2.0","id":")" + id
                  + R"(","method":"job.status","key":")" + key
                  + R"(","params":)" + params + "}");
  };
  status ("1", R"({"job_id":"job-alice"})");                  // no user_id
  status ("2", R"({"job_id":"job-alice","user_id":"bob"})");  // foreign
  status ("3", R"({"job_id":"job-alice","user_id":"alice"})");// owner
  status ("4", R"({"job_id":"job-zero","user_id":"0"})");     // user "0"
  status ("5", R"({"job_id":"nope","user_id":"bob"})");       // missing

  ASSERT_TRUE (wait_gateway (5));
  std::lock_guard<std::mutex> lk (mtx_);
  auto reply = [&] (const std::string &id) -> std::string
  {
    for (const auto &e : gateway_events_)
      if (e.outbound.message.find (R"("id":")" + id + "\"")
          != std::string::npos)
        return e.outbound.message;
    return "";
  };
  EXPECT_NE (reply ("1").find ("-32602"), std::string::npos) << reply ("1");
  EXPECT_NE (reply ("2").find ("-32020"), std::string::npos) << reply ("2");
  EXPECT_NE (reply ("3").find ("\"result\""), std::string::npos) << reply ("3");
  EXPECT_NE (reply ("4").find ("\"result\""), std::string::npos) << reply ("4");
  EXPECT_NE (reply ("4").find (R"("user_id":"0")"), std::string::npos)
    << "status snapshot must carry the owner verbatim: " << reply ("4");
  // Foreign and missing are indistinguishable.
  auto strip_id = [] (std::string m)
  {
    auto p = m.find ("\"id\":\"");
    if (p != std::string::npos)
      m.erase (p, m.find ('"', p + 6) - p + 1);
    return m;
  };
  EXPECT_EQ (strip_id (reply ("2")), strip_id (reply ("5")));
}

// ---------------------------------------------------------------------------
// ADR-039 §H2a: outbox — write the full snapshot, then broadcast
// ---------------------------------------------------------------------------

namespace
{
  std::vector<fs::path> outbox_entries (const fs::path &events_dir)
  {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory (events_dir, ec))
      return out;
    for (const auto &e : fs::directory_iterator (events_dir))
      out.push_back (e.path ());
    return out;
  }

  std::string slurp (const fs::path &p)
  {
    std::ifstream f (p);
    return std::string (std::istreambuf_iterator<char> (f), {});
  }
} // namespace

TEST_F (OrchestratorTest, Outbox_WritesFullSnapshotThenBroadcasts)
{
  const std::string key = insert_key ("operator");
  const fs::path events_dir = home_ / "events";
  fs::remove_all (events_dir);

  Task task;
  task.id = TaskId ("job-outbox");
  task.goal = "g";
  task.user_id = "alice";
  db_->store_job (task);

  send_inbound (R"({"jsonrpc":"2.0","id":"c1","method":"job.cancel","key":")"
                + key + R"(","params":{"job_id":"job-outbox","user_id":"alice"}})");
  ASSERT_TRUE (wait_gateway (2));

  {
    std::lock_guard<std::mutex> lk (mtx_);
    bool broadcast = false;
    for (const auto &e : gateway_events_)
      if (e.outbound.identity.empty ()
          && e.outbound.message.find ("job.phase_changed") != std::string::npos)
      {
        broadcast = true;
        EXPECT_EQ (e.outbound.message.find ("\"steps\""), std::string::npos)
          << "live broadcast stays small";
      }
    EXPECT_TRUE (broadcast);
  }

  const auto entries = outbox_entries (events_dir);
  ASSERT_EQ (entries.size (), 1u) << "exactly one complete file, no temp left";
  const std::string name = entries[0].filename ().string ();
  EXPECT_NE (name.front (), '.');
  EXPECT_EQ (entries[0].extension (), ".json");
  const std::string body = slurp (entries[0]);
  EXPECT_NE (body.find ("\"job.phase_changed\""), std::string::npos) << body;
  EXPECT_NE (body.find (R"("job_id":"job-outbox")"), std::string::npos);
  EXPECT_NE (body.find (R"("user_id":"alice")"), std::string::npos);
  EXPECT_NE (body.find (R"("phase":"cancelled")"), std::string::npos);
  EXPECT_NE (body.find ("\"updated_at\""), std::string::npos);
  EXPECT_NE (body.find ("\"steps\""), std::string::npos);

  EXPECT_TRUE (db_->jobs_pending_notify ().empty ())
    << "notified_seq catches up with state_seq after a successful write";
}

TEST_F (OrchestratorTest, Outbox_WriteFailure_NoBroadcast_ThenReemitted)
{
  const std::string key = insert_key ("operator");
  const fs::path events_dir = home_ / "events";
  fs::remove_all (events_dir);
  { std::ofstream blocker (events_dir); } // a file where the dir should be

  Task task;
  task.id = TaskId ("job-outbox-fail");
  task.goal = "g";
  task.user_id = "0";
  db_->store_job (task);

  send_inbound (R"({"jsonrpc":"2.0","id":"c1","method":"job.cancel","key":")"
                + key
                + R"(","params":{"job_id":"job-outbox-fail","user_id":"0"}})");
  ASSERT_TRUE (wait_gateway (1));
  std::this_thread::sleep_for (std::chrono::milliseconds (100));
  {
    std::lock_guard<std::mutex> lk (mtx_);
    ASSERT_EQ (gateway_events_.size (), 1u) << "only the RPC reply";
    EXPECT_NE (gateway_events_[0].outbound.message.find ("\"ok\":true"),
               std::string::npos);
    gateway_events_.clear ();
  }
  auto pending = db_->jobs_pending_notify ();
  ASSERT_EQ (pending.size (), 1u) << "a failed write leaves the watermark behind";
  EXPECT_EQ (pending[0], "job-outbox-fail");

  // Filesystem recovers; the next heartbeat re-emits the current state.
  fs::remove (events_dir);
  OrchestratorEvent tick;
  tick.kind = OrchestratorEvent::Kind::TimerFired;
  tick.payload_json = R"({"kind":"outbox_reemit"})";
  orch_->enqueue (std::move (tick));
  ASSERT_TRUE (wait_gateway (1));

  const auto entries = outbox_entries (events_dir);
  ASSERT_EQ (entries.size (), 1u);
  const std::string body = slurp (entries[0]);
  EXPECT_NE (body.find (R"("job_id":"job-outbox-fail")"), std::string::npos);
  EXPECT_NE (body.find (R"("phase":"cancelled")"), std::string::npos);
  EXPECT_NE (body.find (R"("user_id":"0")"), std::string::npos);
  EXPECT_TRUE (db_->jobs_pending_notify ().empty ());
}

// ---------------------------------------------------------------------------
// ADR-030: re-registering a package replaces it whole, never in place
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, WorkerRegister_ReplacesPackageAtomically)
{
  const std::string key = insert_key ("admin");
  const fs::path src1 = home_ / "src-v1";
  const fs::path src2 = home_ / "src-v2";
  const std::string manifest
    = R"({"id":"pkg-w","capabilities":[{"method":"pkg.run","description":"d"}]})";
  for (const auto &d : {src1, src2})
  {
    fs::create_directories (d);
    std::ofstream (d / "manifest.json") << manifest;
  }
  std::ofstream (src1 / "worker.py") << "v1";
  std::ofstream (src1 / "stale.txt") << "only in v1";
  std::ofstream (src2 / "worker.py") << "v2";

  auto reg = [&] (const std::string &id, const fs::path &src)
  {
    send_inbound (R"({"jsonrpc":"2.0","id":")" + id
                  + R"(","method":"worker.register","key":")" + key
                  + R"(","params":{"path":")" + src.string () + R"("}})");
  };
  reg ("r1", src1);
  ASSERT_TRUE (wait_gateway (1));
  reg ("r2", src2);
  ASSERT_TRUE (wait_gateway (2));
  {
    std::lock_guard<std::mutex> lk (mtx_);
    for (const auto &e : gateway_events_)
      EXPECT_NE (e.outbound.message.find (R"("worker_id":"pkg-w")"),
                 std::string::npos)
        << e.outbound.message;
  }

  const fs::path workers = home_ / "workers";
  EXPECT_EQ (slurp (workers / "pkg-w" / "worker.py"), "v2");
  EXPECT_FALSE (fs::exists (workers / "pkg-w" / "stale.txt"))
    << "files the new version does not ship must not linger";
  EXPECT_TRUE (outbox_entries (workers / ".staging").empty ());
  const auto parked = outbox_entries (workers / ".old");
  ASSERT_EQ (parked.size (), 1u) << "previous version parked until restart";
  EXPECT_EQ (slurp (parked[0] / "worker.py"), "v1");

  // Registering the installed directory itself is also a whole replace.
  reg ("r3", workers / "pkg-w");
  ASSERT_TRUE (wait_gateway (3));
  {
    std::lock_guard<std::mutex> lk (mtx_);
    EXPECT_NE (gateway_events_.back ().outbound.message.find ("\"result\""),
               std::string::npos)
      << gateway_events_.back ().outbound.message;
  }
  EXPECT_EQ (slurp (workers / "pkg-w" / "worker.py"), "v2");

  // Next daemon start: nothing can run from the parked copy any more.
  orch_->stop ();
  orch_->init ();
  EXPECT_FALSE (fs::exists (workers / ".old"));
  EXPECT_EQ (slurp (workers / "pkg-w" / "worker.py"), "v2");
  orch_->start ();
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
                + key + R"(","params":{"goal":"summarise a document","user_id":"0"}})");

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
// ADR-033 Step 0S / ADR-039 §D2: strict_ability_name
// ---------------------------------------------------------------------------

namespace
{
  int count_jobs (Database &db)
  {
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2 (db.db_handle (), "SELECT COUNT(*) FROM jobs", -1, &st,
                        nullptr);
    int n = -1;
    if (sqlite3_step (st) == SQLITE_ROW)
      n = sqlite3_column_int (st, 0);
    sqlite3_finalize (st);
    return n;
  }
}

TEST_F (OrchestratorTest, StrictAbility_Submit)
{
  const std::string key = insert_key ("operator");
  db_->insert_agent ("ability-ok", "adviser", (home_ / "advisers/ability-ok").string (),
                     "", "an ability");
  db_->insert_agent ("ability-revoked", "adviser",
                     (home_ / "advisers/ability-revoked").string (), "",
                     "a revoked ability");
  registry_->init (*db_);
  // Revoked after the Registry snapshot was taken: the table is
  // authoritative for strict submissions.
  db_->set_worker_enabled ("ability-revoked", false);
  orch_->stop ();
  orch_->init ();
  orch_->start ();

  const int jobs_before = count_jobs (*db_);
  auto submit = [&] (const std::string &id, const std::string &extra)
  {
    send_inbound (R"({"jsonrpc":"2.0","id":")" + id
                  + R"(","method":"job.submit","key":")" + key
                  + R"(","params":{"goal":"g","user_id":"0")" + extra + "}}");
  };
  submit ("missing", R"(,"strict_ability_name":"no-such-ability")");
  submit ("revoked", R"(,"strict_ability_name":"ability-revoked")");
  submit ("both", R"(,"strict_ability_name":"ability-ok","adviser_id":"ability-ok")");
  submit ("ok", R"(,"strict_ability_name":"ability-ok")");

  ASSERT_TRUE (wait_gateway (4));
  ASSERT_TRUE (wait_master (1));
  std::lock_guard<std::mutex> lk (mtx_);
  auto reply = [&] (const std::string &id) -> std::string
  {
    for (const auto &e : gateway_events_)
      if (e.outbound.message.find (R"("id":")" + id + "\"") != std::string::npos)
        return e.outbound.message;
    return "";
  };
  EXPECT_NE (reply ("missing").find ("-32041"), std::string::npos) << reply ("missing");
  EXPECT_NE (reply ("missing").find (R"("ability_name":"no-such-ability")"),
             std::string::npos) << reply ("missing");
  EXPECT_NE (reply ("revoked").find ("-32041"), std::string::npos) << reply ("revoked");
  EXPECT_NE (reply ("both").find ("-32602"), std::string::npos) << reply ("both");
  EXPECT_NE (reply ("ok").find ("\"job_id\""), std::string::npos) << reply ("ok");

  // Only the accepted submission created a job, and it is marked strict.
  EXPECT_EQ (count_jobs (*db_), jobs_before + 1);
  ASSERT_EQ (master_events_.size (), 1u) << "rejected submissions must not reach Master";
  EXPECT_TRUE (db_->job_is_strict_ability (master_events_[0].job_id));
  EXPECT_NE (master_events_[0].payload_json.find (R"("known_adviser_id":"ability-ok")"),
             std::string::npos);
  EXPECT_NE (master_events_[0].payload_json.find (R"("strict_ability":true)"),
             std::string::npos);
}

// ADR-031 §5: a strict-ability job whose Plan names an unregistered Worker
// fails; it never reaches WorkerExhausted / Forge, whatever needs_forge says.
TEST_F (OrchestratorTest, StrictAbility_RegistryMiss_FailsWithoutForge)
{
  for (const bool needs_forge : {false, true})
  {
    const std::string job_id
      = std::string ("job-strict-") + (needs_forge ? "nf" : "plain");
    Task task;
    task.id = TaskId (job_id);
    task.goal = "g";
    db_->store_job (task);
    db_->set_job_strict_ability (job_id);

    {
      std::lock_guard<std::mutex> lk (mtx_);
      master_events_.clear ();
    }
    OrchestratorEvent ev;
    ev.kind = OrchestratorEvent::Kind::MasterDecision;
    ev.job_id = job_id;
    ev.payload_json
      = R"({"type":"plan_ready","job_id":")" + job_id
        + R"(","job_type":"oneshot","steps":[{"id":"step-0","target_type":"worker",)"
          R"("command":"nonexistent.command","description":"d","needs_forge":)"
        + std::string (needs_forge ? "true" : "false") + "}]}";
    orch_->enqueue (std::move (ev));

    bool failed = false;
    for (int i = 0; i < 40 && !failed; ++i)
    {
      std::this_thread::sleep_for (std::chrono::milliseconds (25));
      auto j = db_->load_job (job_id);
      failed = j && j->phase == "failed";
    }
    EXPECT_TRUE (failed) << job_id;
    auto j = db_->load_job (job_id);
    ASSERT_TRUE (j && j->error);
    EXPECT_NE (j->error->find ("nonexistent.command"), std::string::npos);
    std::lock_guard<std::mutex> lk (mtx_);
    for (const auto &me : master_events_)
      EXPECT_NE (me.kind, MasterEvent::Kind::WorkerExhausted)
        << "strict job must not reach WorkerExhausted/Forge";
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

// ---------------------------------------------------------------------------
// ADR-042: Job execution channel -- identity, method whitelist, asset scope
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, JobChannel_ActsForTheJobsUserOnly)
{
  const std::string job_id = "job-chan";
  Task task;
  task.id = TaskId (job_id);
  task.goal = "g";
  task.user_id = "alice";
  db_->store_job (task);
  const fs::path job_dir = home_ / "jobs" / job_id;
  fs::create_directories (job_dir / "output");
  ASSERT_TRUE (UserManager (*db_).register_user ("alice"));

  auto channel = [&] (const std::string &body)
  {
    OrchestratorEvent ev;
    ev.kind = OrchestratorEvent::Kind::ChannelRequest;
    ev.payload_json = body;
    ev.identity = "chan:run-1";
    ev.job_id = job_id;
    ev.channel = ChannelContext{"run-1", job_id, "step-0", "alice",
                                "fact-writer", job_dir.string ()};
    orch_->enqueue (std::move (ev));
  };
  auto settle = [] { std::this_thread::sleep_for (std::chrono::milliseconds (150)); };

  // 1. Recorded for the job's user, attributed to the job.
  channel (R"({"jsonrpc":"2.0","id":"1","method":"user.facts.record","params":)"
           R"({"fact_type":"card_reaction","fact_key":"sku-1","payload":{}}})");
  // 2. A user_id naming someone else is rejected, never honoured.
  channel (R"({"jsonrpc":"2.0","id":"2","method":"user.facts.record","params":)"
           R"({"user_id":"bob","fact_type":"card_reaction","fact_key":"sku-2","payload":{}}})");
  // 3. Methods outside the channel whitelist are unreachable.
  channel (R"({"jsonrpc":"2.0","id":"3","method":"job.cancel","params":)"
           R"({"job_id":"job-chan","user_id":"alice"}})");
  // 4. asset.register: only files inside the job directory.
  std::ofstream (job_dir / "output" / "a.wav") << "RIFF";
  channel (R"({"jsonrpc":"2.0","id":"4","method":"asset.register","params":)"
           R"({"path":")" + (job_dir / "output" / "a.wav").string () + R"("}})");
  std::ofstream (home_ / "secret.txt") << "key";
  channel (R"({"jsonrpc":"2.0","id":"5","method":"asset.register","params":)"
           R"({"path":")" + (home_ / "secret.txt").string () + R"("}})");
  // Symlink escaping the job directory is resolved and refused too.
  fs::create_symlink (home_ / "secret.txt", job_dir / "output" / "link.txt");
  channel (R"({"jsonrpc":"2.0","id":"6","method":"asset.register","params":)"
           R"({"path":")" + (job_dir / "output" / "link.txt").string () + R"("}})");
  settle ();

  auto alice = db_->load_user_fact_events ("alice", std::nullopt, 10);
  ASSERT_EQ (alice.size (), 1u);
  EXPECT_EQ (alice[0].fact_key, "sku-1");
  EXPECT_EQ (alice[0].source, "job:job-chan");
  EXPECT_TRUE (db_->load_user_fact_events ("bob", std::nullopt, 10).empty ());

  auto j = db_->load_job (job_id);
  ASSERT_TRUE (j);
  EXPECT_NE (j->phase, "cancelled") << "job.cancel must not be reachable";

  auto assets = db_->load_assets_for_user ("alice");
  ASSERT_EQ (assets.size (), 1u) << "only the in-job file is registered";
  EXPECT_EQ (assets[0].original_filename, "a.wav");
}

TEST_F (OrchestratorTest, Gateway_FactsRequireExplicitUser)
{
  const std::string key = insert_key ("admin");
  auto call = [&] (const std::string &id, const std::string &params) -> std::string
  {
    {
      std::lock_guard<std::mutex> lk (mtx_);
      gateway_events_.clear ();
    }
    send_inbound (R"({"jsonrpc":"2.0","id":")" + id
                  + R"(","method":"user.facts.record","key":")" + key
                  + R"(","params":)" + params + "}");
    if (!wait_gateway (1))
      return "timeout";
    std::lock_guard<std::mutex> lk (mtx_);
    return gateway_events_[0].outbound.message;
  };
  const std::string no_user = call (
    "1", R"({"fact_type":"card_reaction","fact_key":"k","payload":{}})");
  EXPECT_NE (no_user.find ("-32602"), std::string::npos) << no_user;
  const std::string with_user = call (
    "2", R"({"user_id":"carol","fact_type":"card_reaction","fact_key":"k","payload":{}})");
  EXPECT_NE (with_user.find ("\"result\""), std::string::npos) << with_user;
  auto ev = db_->load_user_fact_events ("carol", std::nullopt, 10);
  ASSERT_EQ (ev.size (), 1u);
  EXPECT_EQ (ev[0].source, "admin:key-admin");
  EXPECT_TRUE (db_->load_user_fact_events ("key-admin", std::nullopt, 10).empty ())
    << "the key id is never used as a user_id";
}

// ADR-030: suite.install is all or nothing. A Suite whose second
// component is invalid must leave the previously installed Adviser package,
// its agents row, and the installed-suite list exactly as they were.
TEST_F (OrchestratorTest, SuiteInstall_FailureRollsBackEverything)
{
  const std::string key = insert_key ("admin");
  auto call = [&] (const std::string &id, const std::string &method,
                   const std::string &params) -> std::string
  {
    {
      std::lock_guard<std::mutex> lk (mtx_);
      gateway_events_.clear ();
    }
    send_inbound (R"({"jsonrpc":"2.0","id":")" + id + R"(","method":")"
                  + method + R"(","key":")" + key + R"(","params":)" + params
                  + "}");
    if (!wait_gateway (1))
      return "timeout";
    std::lock_guard<std::mutex> lk (mtx_);
    return gateway_events_[0].outbound.message;
  };

  // v1 of the Adviser, registered on its own.
  const fs::path v1 = home_ / "adv-v1";
  fs::create_directories (v1);
  std::ofstream (v1 / "manifest.toml")
    << "[meta]\nid = \"suite-a\"\ndescription = \"v1\"\n";
  std::ofstream (v1 / "skill.md") << "v1 skill";
  ASSERT_NE (call ("1", "adviser.register",
                   R"({"path":")" + v1.string () + R"("})")
               .find ("\"result\""),
             std::string::npos);

  // A Suite carrying v2 of that Adviser plus a broken Worker.
  const fs::path suite = home_ / "suite-src";
  fs::create_directories (suite / "advisers" / "suite-a");
  fs::create_directories (suite / "workers" / "broken");
  std::ofstream (suite / "suite.toml")
    << "[meta]\nid = \"suite-a\"\nversion = \"2.0.0\"\n";
  std::ofstream (suite / "advisers" / "suite-a" / "manifest.toml")
    << "[meta]\nid = \"suite-a\"\ndescription = \"v2\"\n";
  std::ofstream (suite / "advisers" / "suite-a" / "skill.md") << "v2 skill";
  std::ofstream (suite / "workers" / "broken" / "manifest.json") << "{ not json";

  const std::string r = call ("2", "suite.install",
                              R"({"path":")" + suite.string () + R"("})");
  EXPECT_NE (r.find ("\"error\""), std::string::npos) << r;

  // Package directory: v1 back in place, nothing of v2 left.
  std::ifstream skill (home_ / "advisers" / "suite-a" / "skill.md");
  std::string content ((std::istreambuf_iterator<char> (skill)), {});
  EXPECT_EQ (content, "v1 skill");
  // Database: agents row still v1, no installed suite recorded.
  auto row = db_->load_agent ("suite-a");
  ASSERT_TRUE (row);
  EXPECT_EQ (row->description, "v1");
  EXPECT_FALSE (db_->load_installed_suite ("suite-a"));
  EXPECT_FALSE (db_->load_agent ("broken"));

  // A valid Suite still installs afterwards (no transaction left open).
  fs::remove_all (suite / "workers");
  const std::string ok = call ("3", "suite.install",
                               R"({"path":")" + suite.string () + R"("})");
  EXPECT_NE (ok.find ("\"result\""), std::string::npos) << ok;
  EXPECT_EQ (db_->load_agent ("suite-a")->description, "v2");
  EXPECT_TRUE (db_->load_installed_suite ("suite-a"));
}

// ADR-031 §14.2: the reviewer flag comes only from an explicit declaration
// and is re-evaluated on every registration.
TEST_F (OrchestratorTest, Register_ReviewerDeclaration)
{
  const std::string key = insert_key ("admin");
  const fs::path w = home_ / "src-rev-w";
  const fs::path a = home_ / "src-rev-a";
  fs::create_directories (w);
  fs::create_directories (a);
  std::ofstream (w / "worker.py") << "";
  std::ofstream (a / "skill.md") << "s";

  auto call = [&] (const std::string &id, const std::string &method,
                   const fs::path &path) -> std::string
  {
    {
      std::lock_guard<std::mutex> lk (mtx_);
      gateway_events_.clear ();
    }
    send_inbound (R"({"jsonrpc":"2.0","id":")" + id + R"(","method":")"
                  + method + R"(","key":")" + key
                  + R"(","params":{"path":")" + path.string () + R"("}})");
    if (!wait_gateway (1))
      return "timeout";
    std::lock_guard<std::mutex> lk (mtx_);
    return gateway_events_[0].outbound.message;
  };
  auto worker_manifest = [&] (const std::string &extra)
  {
    std::ofstream (w / "manifest.json")
      << R"({"id":"rw",)" + extra
           + R"("capabilities":[{"method":"rw.run","description":"d"}]})";
  };
  auto adviser_manifest = [&] (const std::string &extra)
  {
    std::ofstream (a / "manifest.toml")
      << "[meta]\nid = \"ra\"\ndescription = \"d\"\n" + extra;
  };

  worker_manifest (R"("reviewer":true,)");
  EXPECT_NE (call ("1", "worker.register", w).find ("\"result\""), std::string::npos);
  EXPECT_TRUE (db_->agent_is_reviewer ("rw"));
  worker_manifest ("");
  EXPECT_NE (call ("2", "worker.register", w).find ("\"result\""), std::string::npos);
  EXPECT_FALSE (db_->agent_is_reviewer ("rw")) << "dropping the flag clears it";
  worker_manifest (R"("reviewer":"yes",)");
  EXPECT_NE (call ("3", "worker.register", w).find ("-32602"), std::string::npos);

  adviser_manifest ("[review]\nenabled = true\n");
  const auto r = call ("4", "adviser.register", a);
  EXPECT_NE (r.find ("\"result\""), std::string::npos) << r;
  EXPECT_TRUE (db_->agent_is_reviewer ("ra"));
  adviser_manifest ("");
  EXPECT_NE (call ("5", "adviser.register", a).find ("\"result\""), std::string::npos);
  EXPECT_FALSE (db_->agent_is_reviewer ("ra"));
}

// ADR-028: a credential grant records the access key that made it.
TEST_F (OrchestratorTest, CredGrant_RecordsCallerKeyId)
{
  const std::string key = insert_key ("admin");
  send_inbound (R"({"jsonrpc":"2.0","id":"g1","method":"cred.grant","key":")"
                + key + R"(","params":{"worker_id":"w1","provider":"p1"}})");
  ASSERT_TRUE (wait_gateway (1));
  {
    std::lock_guard<std::mutex> lk (mtx_);
    ASSERT_NE (gateway_events_[0].outbound.message.find ("grant_id"),
               std::string::npos)
      << gateway_events_[0].outbound.message;
  }
  auto g = db_->load_credential_grant ("w1", "p1");
  ASSERT_TRUE (g);
  EXPECT_EQ (g->granted_by, "key-admin");
}

// ---------------------------------------------------------------------------
// ADR-031 §13.3: Plan-ingestion validation of step ids and references
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, PlanReady_InvalidReferences_RejectPlan)
{
  struct Case
  {
    const char *name;
    const char *steps;
    const char *expect;
  };
  const Case cases[] = {
    {"nested", R"([{"id":"a","target_type":"worker","command":"x.y","description":"d"},)"
               R"({"id":"b","target_type":"worker","command":"x.y","description":"d",)"
               R"("input":{"v":"$step:a.f.g"}}])",
     "nested path"},
    {"forward", R"([{"id":"a","target_type":"worker","command":"x.y","description":"d",)"
                R"("input":{"v":"$step:b.f"}},)"
                R"({"id":"b","target_type":"worker","command":"x.y","description":"d"}])",
     "does not name an earlier step"},
    {"self", R"([{"id":"a","target_type":"worker","command":"x.y","description":"d",)"
             R"("input":{"v":"$step:a"}}])",
     "does not name an earlier step"},
    {"dup", R"([{"id":"a","target_type":"worker","command":"x.y","description":"d"},)"
            R"({"id":"a","target_type":"worker","command":"x.y","description":"d"}])",
     "duplicate step id"},
    {"badid", R"([{"id":"a.b","target_type":"worker","command":"x.y","description":"d"}])",
     "does not match"},
    {"noid", R"([{"target_type":"worker","command":"x.y","description":"d"}])",
     "has no id"},
    {"asset", R"([{"id":"a","target_type":"worker","command":"x.y","description":"d",)"
              R"("input":{"f":"$asset:not-mine"}}])",
     "not an asset attached"},
  };

  for (const auto &c : cases)
  {
    const std::string job_id = std::string ("job-ref-") + c.name;
    Task task;
    task.id = TaskId (job_id);
    task.goal = "g";
    task.user_id = "0";
    db_->store_job (task);

    OrchestratorEvent ev;
    ev.kind = OrchestratorEvent::Kind::MasterDecision;
    ev.job_id = job_id;
    ev.payload_json = R"({"type":"plan_ready","job_id":")" + job_id
                      + R"(","job_type":"oneshot","steps":)" + c.steps + "}";
    orch_->enqueue (std::move (ev));

    std::optional<Job> j;
    for (int i = 0; i < 80; ++i)
    {
      std::this_thread::sleep_for (std::chrono::milliseconds (10));
      j = db_->load_job (job_id);
      if (j && j->phase == "failed")
        break;
    }
    ASSERT_TRUE (j) << c.name;
    EXPECT_EQ (j->phase, "failed") << c.name;
    ASSERT_TRUE (j->error) << c.name;
    EXPECT_NE (j->error->find ("invalid Plan"), std::string::npos)
      << c.name << ": " << *j->error;
    EXPECT_NE (j->error->find (c.expect), std::string::npos)
      << c.name << ": " << *j->error;
    EXPECT_TRUE (db_->load_steps_for_job (job_id).empty ())
      << c.name << ": a rejected Plan persists no steps";
  }
}

// ADR-031 §13.4: a validated reference that cannot be resolved at run time
// fails the referencing step and the job; no placeholder is substituted.
TEST_F (OrchestratorTest, UnresolvedStepField_FailsStepTerminally)
{
  const fs::path worker_dir = home_ / "workers" / "ref-w";
  fs::create_directories (worker_dir);
  const fs::path script = worker_dir / "worker.sh";
  std::ofstream (script) << "#!/bin/sh\nexit 0\n";
  fs::permissions (script, fs::perms::owner_all);
  db_->insert_agent ("ref-w", "worker", script.string (), "{}");
  db_->insert_capability ("ref-w", "ref.run", "d", "{}");

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

  const std::string job_id = "job-unresolved";
  Task task;
  task.id = TaskId (job_id);
  task.goal = "g";
  task.user_id = "0";
  db_->store_job (task);

  OrchestratorEvent ev;
  ev.kind = OrchestratorEvent::Kind::MasterDecision;
  ev.job_id = job_id;
  ev.payload_json
    = R"({"type":"plan_ready","job_id":")" + job_id
      + R"(","job_type":"oneshot","steps":[)"
        R"({"id":"s0","target_type":"worker","command":"ref.run","description":"d"},)"
        R"({"id":"s1","target_type":"worker","command":"ref.run","description":"d",)"
        R"("input":{"v":"$step:s0.missing"}}]})";
  orch_->enqueue (std::move (ev));

  bool running = false;
  for (int i = 0; i < 80 && !running; ++i)
  {
    std::this_thread::sleep_for (std::chrono::milliseconds (25));
    for (const auto &st : db_->load_steps_for_job (job_id))
      running = running || (st.id == "s0" && st.status == "running");
  }
  ASSERT_TRUE (running);

  const fs::path run_dir = home_ / "fake-run-unresolved";
  fs::create_directories (run_dir);
  std::ofstream (run_dir / "result.json") << R"({"status":"ok","result":{"a":1}})";
  OrchestratorEvent done;
  done.kind = OrchestratorEvent::Kind::WorkerDone;
  done.job_id = job_id;
  done.payload_json = R"({"run_id":"fake","exit_code":0,"run_dir":")"
                      + run_dir.string () + R"("})";
  orch_->enqueue (std::move (done));

  std::optional<Job> j;
  for (int i = 0; i < 80; ++i)
  {
    std::this_thread::sleep_for (std::chrono::milliseconds (25));
    j = db_->load_job (job_id);
    if (j && j->phase == "failed")
      break;
  }
  ASSERT_TRUE (j);
  EXPECT_EQ (j->phase, "failed");
  ASSERT_TRUE (j->error);
  EXPECT_NE (j->error->find ("unresolved reference $step:s0.missing"),
             std::string::npos)
    << *j->error;
  for (const auto &st : db_->load_steps_for_job (job_id))
  {
    if (st.id == "s0")
      EXPECT_EQ (st.status, "done");
    if (st.id == "s1")
      EXPECT_EQ (st.status, "failed");
  }
}

// ---------------------------------------------------------------------------
// ADR-031 §14 / ADR-016: reviewer Workers
// ---------------------------------------------------------------------------

TEST_F (OrchestratorTest, ReviewerWorker_VerdictHandling)
{
  const std::string key = insert_key ("operator");
  const fs::path worker_dir = home_ / "workers" / "rev-w";
  fs::create_directories (worker_dir);
  const fs::path script = worker_dir / "worker.sh";
  std::ofstream (script) << "#!/bin/sh\nexit 0\n";
  fs::permissions (script, fs::perms::owner_all);
  db_->insert_agent ("rev-w", "worker", script.string (), "{}");
  db_->insert_capability ("rev-w", "review.check", "d", "{}");
  db_->set_agent_reviewer ("rev-w", true);
  db_->insert_agent ("plain-w", "worker", script.string (), "{}");
  db_->insert_capability ("plain-w", "plain.check", "d", "{}");
  EXPECT_TRUE (db_->agent_is_reviewer ("rev-w"));
  EXPECT_FALSE (db_->agent_is_reviewer ("plain-w"));

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

  // Runs one single-step job and feeds the given result.json to its step
  // as if the Worker had exited 0. Returns the terminal phase.
  auto run = [&] (const std::string &job_id, const std::string &command,
                  const std::string &result_file) -> std::string
  {
    Task task;
    task.id = TaskId (job_id);
    task.goal = "g";
    task.user_id = "0";
    db_->store_job (task);

    OrchestratorEvent ev;
    ev.kind = OrchestratorEvent::Kind::MasterDecision;
    ev.job_id = job_id;
    ev.payload_json = R"({"type":"plan_ready","job_id":")" + job_id
                      + R"(","job_type":"oneshot","steps":[{"id":"step-0",)"
                        R"("target_type":"worker","command":")"
                      + command + R"(","description":"d"}]})";
    orch_->enqueue (std::move (ev));

    bool running = false;
    for (int i = 0; i < 80 && !running; ++i)
    {
      std::this_thread::sleep_for (std::chrono::milliseconds (25));
      for (const auto &st : db_->load_steps_for_job (job_id))
        running = running || st.status == "running";
    }
    if (!running)
      return "not dispatched";

    const fs::path run_dir = home_ / ("fake-run-" + job_id);
    fs::create_directories (run_dir);
    std::ofstream (run_dir / "result.json") << result_file;

    OrchestratorEvent done;
    done.kind = OrchestratorEvent::Kind::WorkerDone;
    done.job_id = job_id;
    done.payload_json = R"({"run_id":"fake","exit_code":0,"run_dir":")"
                        + run_dir.string () + R"("})";
    orch_->enqueue (std::move (done));

    for (int i = 0; i < 80; ++i)
    {
      std::this_thread::sleep_for (std::chrono::milliseconds (25));
      auto j = db_->load_job (job_id);
      if (j && (j->phase == "done" || j->phase == "failed"))
        return j->phase;
    }
    return "stuck";
  };

  // Declared reviewer, "rejected" + verdict reject → review_rejected.
  EXPECT_EQ (run ("job-rev-reject", "review.check",
                  R"({"status":"rejected","result":{"review":{"verdict":"reject",)"
                  R"("findings":[{"code":"c1","message":"m1"}]}}})"),
             "failed");
  {
    auto f = db_->load_job_failure ("job-rev-reject");
    ASSERT_TRUE (f);
    EXPECT_EQ (f->kind, "review_rejected");
    EXPECT_EQ (f->review_json,
               R"({"step_id":"step-0","reviewer_id":"rev-w","verdict":"reject",)"
               R"("findings":[{"code":"c1","message":"m1"}]})");
    auto steps = db_->load_steps_for_job ("job-rev-reject");
    ASSERT_EQ (steps.size (), 1u);
    EXPECT_EQ (steps[0].status, "failed");
    EXPECT_NE (steps[0].result_json.find ("\"verdict\":\"reject\""),
               std::string::npos)
      << "the reviewer's result is persisted";
    auto j = db_->load_job ("job-rev-reject");
    ASSERT_TRUE (j && j->error);
    EXPECT_EQ (*j->error, "review rejected by rev-w");

    // Wire shape (ADR-039 §B).
    {
      std::lock_guard<std::mutex> lk (mtx_);
      gateway_events_.clear ();
    }
    send_inbound (R"({"jsonrpc":"2.0","id":"s1","method":"job.status","key":")"
                  + key + R"(","params":{"job_id":"job-rev-reject","user_id":"0"}})");
    ASSERT_TRUE (wait_gateway (1));
    std::lock_guard<std::mutex> lk (mtx_);
    const std::string &m = gateway_events_[0].outbound.message;
    EXPECT_NE (m.find (R"("failure_kind":"review_rejected")"), std::string::npos) << m;
    EXPECT_NE (m.find (R"("review":{"step_id":"step-0","reviewer_id":"rev-w")"),
               std::string::npos) << m;
  }

  // Declared reviewer, "ok" + verdict pass → done, no failure_kind.
  EXPECT_EQ (run ("job-rev-pass", "review.check",
                  R"({"status":"ok","result":{"review":{"verdict":"pass"},"x":1}})"),
             "done");
  EXPECT_FALSE (db_->load_job_failure ("job-rev-pass"));

  // Declared reviewer, "ok" without a review object → invalid output.
  EXPECT_EQ (run ("job-rev-noreview", "review.check",
                  R"({"status":"ok","result":{"x":1}})"),
             "failed");
  EXPECT_FALSE (db_->load_job_failure ("job-rev-noreview"));

  // Not a reviewer: "rejected" is a Worker Contract violation, not a verdict.
  EXPECT_EQ (run ("job-plain-reject", "plain.check",
                  R"({"status":"rejected","result":{"review":{"verdict":"reject"}}})"),
             "failed");
  EXPECT_FALSE (db_->load_job_failure ("job-plain-reject"));

  // A job that failed for another reason reports failure_kind null.
  {
    std::lock_guard<std::mutex> lk (mtx_);
    gateway_events_.clear ();
  }
  send_inbound (R"({"jsonrpc":"2.0","id":"s2","method":"job.status","key":")"
                + key + R"(","params":{"job_id":"job-plain-reject","user_id":"0"}})");
  ASSERT_TRUE (wait_gateway (1));
  std::lock_guard<std::mutex> lk (mtx_);
  const std::string &m = gateway_events_[0].outbound.message;
  EXPECT_NE (m.find (R"("failure_kind":null)"), std::string::npos) << m;
  EXPECT_EQ (m.find (R"("review":)"), std::string::npos) << m;
}

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


// ADR-041: the service role and user-bound keys.
TEST_F (OrchestratorTest, ServiceRole_UserBoundKeys)
{
  const std::string admin = insert_key ("admin");
  const std::string svc = insert_key ("service"); // unbound management key
  auto call = [&] (const std::string &key, const std::string &method,
                   const std::string &params) -> std::string
  {
    {
      std::lock_guard<std::mutex> lk (mtx_);
      gateway_events_.clear ();
    }
    send_inbound (R"({"jsonrpc":"2.0","id":"t","method":")" + method
                  + R"(","key":")" + key + R"(","params":)" + params + "}");
    if (!wait_gateway (1))
      return "timeout";
    std::lock_guard<std::mutex> lk (mtx_);
    return gateway_events_[0].outbound.message;
  };
  auto ok = [] (const std::string &r)
  { return r.find ("\"result\"") != std::string::npos; };
  auto err = [] (const std::string &r, const char *code)
  { return r.find (code) != std::string::npos && r.find ("\"error\"") != std::string::npos; };
  auto issued_key = [] (const std::string &r) -> std::string
  {
    rapidjson::Document d;
    d.Parse (r.c_str ());
    if (d.HasParseError () || !d.HasMember ("result")
        || !d["result"].HasMember ("access_key"))
      return {};
    return d["result"]["access_key"]["key"].GetString ();
  };

  // First registration issues a bound key; a repeat does not.
  const std::string r1 = call (svc, "user.register", R"({"user_id":"alice"})");
  const std::string alice = issued_key (r1);
  ASSERT_EQ (alice.rfind ("ak_", 0), 0u) << r1;
  EXPECT_EQ (alice.size (), 67u);
  EXPECT_TRUE (issued_key (call (svc, "user.register", R"({"user_id":"alice"})")).empty ());
  const std::string bob = issued_key (call (admin, "user.register", R"({"user_id":"bob"})"));
  ASSERT_FALSE (bob.empty ());
  EXPECT_TRUE (err (call (svc, "user.register", R"({"user_id":""})"), "-32602"));

  // Hash-only storage for bound keys.
  int bound = 0;
  for (const auto &k : db_->load_active_access_keys ())
    if (k.user_id)
    {
      ++bound;
      EXPECT_TRUE (k.key.empty ()) << "bound keys are stored hash-only";
      EXPECT_EQ (k.role, "service");
    }
  EXPECT_EQ (bound, 2);

  // Unbound service key: management and catalogue only.
  EXPECT_TRUE (ok (call (svc, "worker.list", "{}")));
  EXPECT_TRUE (err (call (svc, "job.list", R"({"user_id":"alice"})"), "-32011"));
  EXPECT_TRUE (err (call (svc, "worker.register", R"({"path":"/x"})"), "-32011"));
  EXPECT_TRUE (err (call (svc, "user.disable", R"({"user_id":"alice"})"), "-32011"));

  // Bound key: acts for its user only.
  EXPECT_TRUE (ok (call (alice, "job.list", "{}"))) << "user_id is pinned";
  EXPECT_TRUE (ok (call (alice, "job.list", R"({"user_id":"alice"})")));
  EXPECT_TRUE (ok (call (alice.substr (3), "job.list", "{}"))) << "ak_ is optional";
  EXPECT_TRUE (err (call (alice, "job.list", R"({"user_id":"bob"})"), "-32011"));
  EXPECT_TRUE (err (call (alice, "job.list", R"({"all_users":true})"), "-32011"));
  EXPECT_TRUE (ok (call (alice, "cred.list", "{}")));
  EXPECT_TRUE (err (call (alice, "user.register", R"({"user_id":"mallory"})"), "-32011"));
  EXPECT_TRUE (err (call (alice, "user.facts.record",
                          R"({"fact_type":"card_reaction","fact_key":"k","payload":{}})"),
                    "-32011"));
  EXPECT_TRUE (err (call (alice, "user.key.issue", R"({"user_id":"alice"})"), "-32011"));

  // Rotation revokes the old key.
  const std::string r2 = call (svc, "user.key.issue",
                               R"({"user_id":"alice","revoke_existing":true})");
  const std::string alice2 = issued_key (r2);
  ASSERT_FALSE (alice2.empty ()) << r2;
  EXPECT_TRUE (err (call (alice, "job.list", "{}"), "-32010"));
  EXPECT_TRUE (ok (call (alice2, "job.list", "{}")));

  // A key can only be revoked through its own user.
  rapidjson::Document d2;
  d2.Parse (r2.c_str ());
  const std::string alice2_id = d2["result"]["access_key"]["key_id"].GetString ();
  EXPECT_TRUE (err (call (svc, "user.key.revoke",
                          R"({"user_id":"bob","key_id":")" + alice2_id + R"("})"),
                    "-32020"));

  // Disabling a user revokes its keys; no new key can be issued.
  EXPECT_TRUE (ok (call (admin, "user.disable", R"({"user_id":"alice"})")));
  EXPECT_TRUE (err (call (alice2, "job.list", "{}"), "-32010"));
  EXPECT_TRUE (err (call (svc, "user.key.issue", R"({"user_id":"alice"})"), "-32020"));
  EXPECT_TRUE (ok (call (bob, "job.list", "{}"))) << "other users unaffected";
}

// ADR-041 §9: user-bound keys reach the daemon's filesystem only through
// inbox/<user_id>/ (asset.register) and export/<user_id>/ (asset.extract).
TEST_F (OrchestratorTest, ServiceRole_AssetPathsAreConfined)
{
  const std::string svc = insert_key ("service");
  auto call = [&] (const std::string &key, const std::string &method,
                   const std::string &params) -> std::string
  {
    {
      std::lock_guard<std::mutex> lk (mtx_);
      gateway_events_.clear ();
    }
    send_inbound (R"({"jsonrpc":"2.0","id":"t","method":")" + method
                  + R"(","key":")" + key + R"(","params":)" + params + "}");
    if (!wait_gateway (1))
      return "timeout";
    std::lock_guard<std::mutex> lk (mtx_);
    return gateway_events_[0].outbound.message;
  };
  auto field = [] (const std::string &r, const char *a, const char *b = nullptr)
  {
    rapidjson::Document d;
    d.Parse (r.c_str ());
    if (d.HasParseError () || !d.HasMember ("result"))
      return std::string{};
    const auto &v = b ? d["result"][a][b] : d["result"][a];
    return std::string (v.GetString ());
  };
  auto has = [] (const std::string &r, const char *s)
  { return r.find (s) != std::string::npos; };

  const std::string alice
    = field (call (svc, "user.register", R"({"user_id":"alice"})"), "access_key", "key");
  const std::string bob
    = field (call (svc, "user.register", R"({"user_id":"bob"})"), "access_key", "key");
  ASSERT_FALSE (alice.empty ());
  ASSERT_FALSE (bob.empty ());

  const fs::path inbox = home_ / "inbox" / "alice";
  fs::create_directories (inbox);
  std::ofstream (inbox / "a.txt") << "hello";
  std::ofstream (home_ / "secret.txt") << "daemon-only";

  // register: only from the user's own inbox.
  const std::string r1 = call (alice, "asset.register",
                               R"({"path":")" + (inbox / "a.txt").string ()
                                 + R"(","filename":"worker.py"})");
  const std::string asset_id = field (r1, "asset_id");
  ASSERT_FALSE (asset_id.empty ()) << r1;
  EXPECT_TRUE (has (call (alice, "asset.register",
                          R"({"path":")" + (home_ / "secret.txt").string () + R"("})"),
                    "-32011"));
  EXPECT_TRUE (has (call (alice, "asset.register",
                          R"({"path":")" + (inbox / ".." / ".." / "secret.txt").string ()
                            + R"("})"),
                    "-32011"));
  fs::create_symlink (home_ / "secret.txt", inbox / "link.txt");
  EXPECT_TRUE (has (call (alice, "asset.register",
                          R"({"path":")" + (inbox / "link.txt").string () + R"("})"),
                    "-32011"))
    << "symlinks are resolved before the check";

  // extract: default and explicit destinations under export/alice only.
  const std::string e1
    = call (alice, "asset.extract", R"({"asset_id":")" + asset_id + R"("})");
  EXPECT_TRUE (has (e1, "\"result\"")) << e1;
  EXPECT_TRUE (fs::exists (home_ / "export" / "alice" / "worker.py"));
  EXPECT_TRUE (has (call (alice, "asset.extract",
                          R"({"asset_id":")" + asset_id + R"(","dest_dir":")"
                            + (home_ / "export" / "alice" / "sub").string () + R"("})"),
                    "\"result\""));
  const fs::path workers = home_ / "workers" / "victim";
  EXPECT_TRUE (has (call (alice, "asset.extract",
                          R"({"asset_id":")" + asset_id + R"(","dest_dir":")"
                            + workers.string () + R"("})"),
                    "-32011"));
  EXPECT_FALSE (fs::exists (workers)) << "nothing is created outside export/";
  EXPECT_TRUE (has (call (alice, "asset.extract",
                          R"({"asset_id":")" + asset_id + R"(","dest_dir":")"
                            + (home_ / "export" / "alice" / ".." / "bob").string ()
                            + R"("})"),
                    "-32011"));
  fs::create_directories (home_ / "elsewhere");
  fs::create_directory_symlink (home_ / "elsewhere", home_ / "export" / "alice" / "out");
  EXPECT_TRUE (has (call (alice, "asset.extract",
                          R"({"asset_id":")" + asset_id + R"(","dest_dir":")"
                            + (home_ / "export" / "alice" / "out").string () + R"("})"),
                    "-32011"));
  EXPECT_FALSE (fs::exists (home_ / "elsewhere" / "worker.py"));
  EXPECT_TRUE (has (call (bob, "asset.extract",
                          R"({"asset_id":")" + asset_id + R"("})"),
                    "-32020"));

  // revoke_by_user: own user only.
  EXPECT_TRUE (has (call (alice, "asset.revoke_by_user", R"({"user_id":"bob"})"),
                    "-32011"));
  EXPECT_TRUE (has (call (alice, "asset.revoke_by_user", "{}"), "\"result\""));
  EXPECT_TRUE (has (call (alice, "asset.show", R"({"asset_id":")" + asset_id + R"("})"),
                    "-32020"));
}
