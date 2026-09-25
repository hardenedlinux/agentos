/**
 * tests/test_periodic_executor.cpp
 *
 * Unit tests for PeriodicExecutor (ADR-023).
 */

#include "agentos/periodic_executor.h"
#include "agentos/database.h"
#include "agentos/config.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <memory>
#include <sqlite3.h>
#include <string>

namespace {

// -------------------------------------------------------------------
// Fixture
// -------------------------------------------------------------------
class PeriodicExecutorTest : public ::testing::Test {
protected:
  void SetUp () override {
    // Open an in‑memory SQLite database.
    db_ = std::make_unique<agentos::Database> (":memory:");
    ASSERT_TRUE (db_->open ());

    // Minimal config — heartbeat interval of 30 s.
    config_.gateway.heartbeat_interval_s = 30;

    // Install recording callbacks for the three dispatch targets.
    send_to_orchestrator_ =
      [this] (agentos::OrchestratorEvent ev) {
        last_orchestrator_ = std::move (ev);
      };
    send_to_master_ =
      [this] (agentos::MasterEvent ev) {
        last_master_ = std::move (ev);
      };
    gateway_push_ =
      [this] (const std::string &p) {
        last_gateway_payload_ = p;
      };

    // We use a real agentos::Dispatcher (default-constructible) because
    // PeriodicExecutor stores a reference to it.  The reaper path is
    // exercised in higher-level tests and is harmless when there are no
    // in-flight processes.
  }

  void TearDown () override {
    if (db_)
      db_->close ();
  }

  void create_executor () {
    executor_ = std::make_unique<agentos::PeriodicExecutor> (
      *db_, dispatcher_, config_,
      send_to_orchestrator_,
      send_to_master_,
      gateway_push_);
  }

  std::unique_ptr<agentos::Database> db_;
  agentos::Dispatcher                dispatcher_;   // default-constructible
  agentos::Config                    config_;

  agentos::OrchestratorEvent last_orchestrator_;
  agentos::MasterEvent       last_master_;
  std::string                last_gateway_payload_;

  agentos::PeriodicExecutor::SendToOrchestrator send_to_orchestrator_;
  agentos::PeriodicExecutor::SendToMaster       send_to_master_;
  agentos::PeriodicExecutor::GatewayPush        gateway_push_;

  std::unique_ptr<agentos::PeriodicExecutor> executor_;
};

// -------------------------------------------------------------------
// Helper – current Unix time in seconds
// -------------------------------------------------------------------
static int64_t unix_now_s () {
  using namespace std::chrono;
  return static_cast<int64_t> (
    duration_cast<seconds> (system_clock::now ().time_since_epoch ())
      .count ());
}

// -------------------------------------------------------------------
// 1. Heartbeat is seeded when absent
// -------------------------------------------------------------------
TEST_F (PeriodicExecutorTest, HeartbeatSeededWhenAbsent) {
  create_executor ();
  executor_->init ();

  sqlite3 *db = db_->db_handle ();
  ASSERT_NE (db, nullptr);

  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (
               db,
               "SELECT interval_s, next_fire, target FROM timer_tasks "
               "WHERE id = 'heartbeat' AND enabled = 1",
               -1, &stmt, nullptr),
             SQLITE_OK);

  ASSERT_EQ (sqlite3_step (stmt), SQLITE_ROW);
  EXPECT_EQ (sqlite3_column_int64 (stmt, 0), 30);

  int64_t now = unix_now_s ();
  int64_t nf  = sqlite3_column_int64 (stmt, 1);
  EXPECT_LE (std::abs (nf - (now + 30)), 5);

  EXPECT_STREQ (
    reinterpret_cast<const char *> (sqlite3_column_text (stmt, 2)),
    "gateway");

  sqlite3_finalize (stmt);
}

// -------------------------------------------------------------------
// 2. Already‑seeded heartbeat is not overwritten
// -------------------------------------------------------------------
TEST_F (PeriodicExecutorTest, HeartbeatNotReSeededIfPresent) {
  sqlite3 *db = db_->db_handle ();
  ASSERT_NE (db, nullptr);

  int64_t now = unix_now_s ();
  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (
               db,
               "INSERT INTO timer_tasks "
               "(id, interval_s, next_fire, target, payload_json, enabled, "
               "created_at) "
               "VALUES ('heartbeat', 10, ?, 'gateway', '', 1, ?)",
               -1, &stmt, nullptr),
             SQLITE_OK);
  sqlite3_bind_int64 (stmt, 1, now + 100);
  sqlite3_bind_int64 (stmt, 2, now);
  ASSERT_EQ (sqlite3_step (stmt), SQLITE_DONE);
  sqlite3_finalize (stmt);

  create_executor ();
  executor_->init ();

  sqlite3_stmt *q = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (
               db,
               "SELECT interval_s, next_fire FROM timer_tasks "
               "WHERE id = 'heartbeat' AND enabled = 1",
               -1, &q, nullptr),
             SQLITE_OK);
  ASSERT_EQ (sqlite3_step (q), SQLITE_ROW);
  EXPECT_EQ (sqlite3_column_int64 (q, 0), 10);
  EXPECT_EQ (sqlite3_column_int64 (q, 1), now + 100);
  sqlite3_finalize (q);
}

// -------------------------------------------------------------------
// 3. Expired one‑shot tasks are disabled on restart
// -------------------------------------------------------------------
TEST_F (PeriodicExecutorTest, LoadExpiredOneShotDisabled) {
  sqlite3 *db = db_->db_handle ();
  ASSERT_NE (db, nullptr);

  int64_t now = unix_now_s ();
  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (
               db,
               "INSERT INTO timer_tasks "
               "(id, interval_s, next_fire, target, payload_json, enabled, "
               "created_at) "
               "VALUES ('oneshot', 0, ?, 'orchestrator', '{}', 1, ?)",
               -1, &stmt, nullptr),
             SQLITE_OK);
  sqlite3_bind_int64 (stmt, 1, now - 10);
  sqlite3_bind_int64 (stmt, 2, now - 20);
  ASSERT_EQ (sqlite3_step (stmt), SQLITE_DONE);
  sqlite3_finalize (stmt);

  create_executor ();
  executor_->init ();

  sqlite3_stmt *q = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (
               db,
               "SELECT enabled FROM timer_tasks WHERE id = 'oneshot'",
               -1, &q, nullptr),
             SQLITE_OK);
  ASSERT_EQ (sqlite3_step (q), SQLITE_ROW);
  EXPECT_EQ (sqlite3_column_int (q, 0), 0);
  sqlite3_finalize (q);
}

// -------------------------------------------------------------------
// 4. Missed periodic tasks have next_fire advanced
// -------------------------------------------------------------------
TEST_F (PeriodicExecutorTest, LoadMissedPeriodicAdvancesNextFire) {
  sqlite3 *db = db_->db_handle ();
  ASSERT_NE (db, nullptr);

  int64_t now = unix_now_s ();
  constexpr int64_t interval = 20;

  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (
               db,
               "INSERT INTO timer_tasks "
               "(id, interval_s, next_fire, target, payload_json, enabled, "
               "created_at) "
               "VALUES ('per', ?, ?, 'master', '{}', 1, ?)",
               -1, &stmt, nullptr),
             SQLITE_OK);
  sqlite3_bind_int64 (stmt, 1, interval);
  sqlite3_bind_int64 (stmt, 2, now - 5); // missed by 5 seconds
  sqlite3_bind_int64 (stmt, 3, now - 10);
  ASSERT_EQ (sqlite3_step (stmt), SQLITE_DONE);
  sqlite3_finalize (stmt);

  create_executor ();
  executor_->init ();

  sqlite3_stmt *q = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (
               db,
               "SELECT next_fire FROM timer_tasks WHERE id = 'per' AND enabled = 1",
               -1, &q, nullptr),
             SQLITE_OK);
  ASSERT_EQ (sqlite3_step (q), SQLITE_ROW);
  int64_t nf = sqlite3_column_int64 (q, 0);
  EXPECT_LE (std::abs (nf - (now + interval)), 5);
  sqlite3_finalize (q);
}

// -------------------------------------------------------------------
// 5. register_task persists row and places it on the heap
// -------------------------------------------------------------------
TEST_F (PeriodicExecutorTest, RegisterTaskPersistsRow) {
  create_executor ();
  executor_->init ();

  agentos::PeriodicControl::Task t;
  t.id           = "custom";
  t.interval_s   = 60;
  t.next_fire    = unix_now_s () + 120;
  t.target       = agentos::TaskTarget::Master;
  t.payload_json = R"({"key":"val"})";

  executor_->test_register_task (t);

  // Verify DB row
  sqlite3 *db = db_->db_handle ();
  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (
               db,
               "SELECT interval_s, target, payload_json FROM timer_tasks "
               "WHERE id = 'custom' AND enabled = 1",
               -1, &stmt, nullptr),
             SQLITE_OK);
  ASSERT_EQ (sqlite3_step (stmt), SQLITE_ROW);
  EXPECT_EQ (sqlite3_column_int64 (stmt, 0), 60);
  EXPECT_STREQ (
    reinterpret_cast<const char *> (sqlite3_column_text (stmt, 1)),
    "master");
  EXPECT_STREQ (
    reinterpret_cast<const char *> (sqlite3_column_text (stmt, 2)),
    R"({"key":"val"})");
  sqlite3_finalize (stmt);

  // Heap must contain the task
  EXPECT_GE (executor_->heap_size (), 1U);
}

// -------------------------------------------------------------------
// 6. cancel_task marks disabled and removes from heap
// -------------------------------------------------------------------
TEST_F (PeriodicExecutorTest, CancelTaskMarksDisabled) {
  create_executor ();
  executor_->init ();

  {
    agentos::PeriodicControl::Task t;
    t.id         = "temp";
    t.interval_s = 1;
    t.next_fire  = unix_now_s () + 10;
    t.target     = agentos::TaskTarget::Gateway;
    t.payload_json = "nothing";
    executor_->test_register_task (t);
  }

  auto before = executor_->heap_size ();
  ASSERT_GE (before, 1U); // at least the registered task + possibly heartbeat

  executor_->test_cancel_task ("temp");

  // DB mark disabled
  sqlite3 *db = db_->db_handle ();
  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (
               db,
               "SELECT enabled FROM timer_tasks WHERE id = 'temp'",
               -1, &stmt, nullptr),
             SQLITE_OK);
  ASSERT_EQ (sqlite3_step (stmt), SQLITE_ROW);
  EXPECT_EQ (sqlite3_column_int (stmt, 0), 0);
  sqlite3_finalize (stmt);

  // Heap size decreased by 1
  EXPECT_EQ (executor_->heap_size (), before - 1);
}

// -------------------------------------------------------------------
// 7. fire dispatches to the correct callback (Gateway)
// -------------------------------------------------------------------
TEST_F (PeriodicExecutorTest, FireGatewayTask) {
  create_executor ();
  executor_->init ();

  agentos::PeriodicExecutor::Task t;
  t.id           = "gw";
  t.next_fire    = 0; // not used by fire
  t.interval_s   = 0;
  t.target       = agentos::TaskTarget::Gateway;
  t.payload_json = R"({"hello":"world"})";

  executor_->test_fire (t);

  EXPECT_EQ (last_gateway_payload_, R"({"hello":"world"})");
}

// -------------------------------------------------------------------
// 8. fire heartbeat builds live payload and pushes gateway
// -------------------------------------------------------------------
TEST_F (PeriodicExecutorTest, FireHeartbeatBuildsLivePayload) {
  create_executor ();
  executor_->init ();

  agentos::PeriodicExecutor::Task t;
  t.id          = "heartbeat";
  t.target      = agentos::TaskTarget::Gateway;
  t.payload_json = ""; // must be built at fire time

  executor_->test_fire (t);

  // The payload must be a JSON‑RPC heartbeat notification.
  EXPECT_NE (last_gateway_payload_.find ("\"system.heartbeat\""),
             std::string::npos);
  EXPECT_NE (last_gateway_payload_.find ("\"running_jobs\""),
             std::string::npos);
}

// -------------------------------------------------------------------
// 9. fire dispatches to Orchestrator
// -------------------------------------------------------------------
TEST_F (PeriodicExecutorTest, FireOrchestratorTask) {
  create_executor ();
  executor_->init ();

  agentos::PeriodicExecutor::Task t;
  t.id           = "orch";
  t.target       = agentos::TaskTarget::Orchestrator;
  t.payload_json = R"({"kind":"scheduled_job_fire"})";

  executor_->test_fire (t);

  EXPECT_EQ (last_orchestrator_.kind,
             agentos::OrchestratorEvent::Kind::TimerFired);
  EXPECT_EQ (last_orchestrator_.payload_json,
             R"({"kind":"scheduled_job_fire"})");
}

// -------------------------------------------------------------------
// 10. fire dispatches to Master
// -------------------------------------------------------------------
TEST_F (PeriodicExecutorTest, FireMasterTask) {
  create_executor ();
  executor_->init ();

  agentos::PeriodicExecutor::Task t;
  t.id           = "mst";
  t.target       = agentos::TaskTarget::Master;
  t.payload_json = R"({"action":"review"})";

  executor_->test_fire (t);

  EXPECT_EQ (last_master_.kind,
             agentos::MasterEvent::Kind::ScheduledTask);
  EXPECT_EQ (last_master_.payload_json,
             R"({"action":"review"})");
}

} // anonymous namespace
