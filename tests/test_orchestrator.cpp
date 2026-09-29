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

