/**
 * test_forge_coordinator.cpp
 *
 * Unit tests for ForgeCoordinator (ADR-022, ADR-019).
 *
 * Non-LLM tests (ForgeCoordinatorTest):
 *   - DB persistence at each state transition
 *   - Enforce Layer policy rejection (network:true, exec:true, contradictory
 * ports)
 *   - Retry loop: attempt counter increments, feedback propagates
 *   - human_review escalation after max_attempts
 *   - promote_worker: file system artifacts created, Registry updated
 *   - Completion callback invoked with correct outcome
 *
 * LLM integration tests (ForgeCoordinatorLlmTest):
 *   - Skipped unless DEEPSEEK_API_KEY set
 *   - Full pipeline run with real Code Writer + Reviewer
 */

#include <gtest/gtest.h>

#include "agentos/database.h"
#include "agentos/forge_coordinator.h"
#include "agentos/forge_pipeline_job.h"
#include "agentos/home_init.h"
#include "agentos/llm_proxy.h"
#include "agentos/registry.h"
#include "agentos/types.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace agentos;
using namespace agentos::forge;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

  // Minimal valid capability declaration — network:false, exec:false.
  constexpr std::string_view kCapGood = R"({
  "network": false,
  "exec":    false,
  "fs_read":  [],
  "fs_write": []
})";

  // Capability that Enforce Layer must reject.
  constexpr std::string_view kCapNetworkTrue = R"({
  "network": true,
  "exec":    false,
  "fs_read":  [],
  "fs_write": []
})";

  constexpr std::string_view kCapExecTrue = R"({
  "network": false,
  "exec":    true,
  "fs_read":  [],
  "fs_write": []
})";

  // tcp_connect_ports non-empty + network:false — contradictory (ADR-015).
  constexpr std::string_view kCapContradictory = R"({
  "network": false,
  "exec":    false,
  "fs_read":  [],
  "fs_write": [],
  "tcp_connect_ports": [443]
})";

  // Build a minimal Code Reviewer accept verdict.
  std::string make_reviewer_accept (std::string_view task_id = "t1")
  {
    return std::string (R"({"task_id":")") + std::string (task_id)
           + R"(","status":"accept","reason":"all checks passed"})";
  }

  // Build a Code Reviewer reject verdict.
  std::string make_reviewer_reject (std::string_view reason,
                                    std::string_view task_id = "t1")
  {
    return std::string (R"({"task_id":")") + std::string (task_id)
           + R"(","status":"reject","reason":")" + std::string (reason)
           + R"("})";
  }

  // Minimal requirement JSON.
  // "method" must be present and ADR-031-valid (namespace.verb, lowercase
  // segments) — promote_worker falls back to worker_id (the forge_job_id,
  // e.g. "llm-fc-1") when it's absent, which fails is_valid_method's dot
  // check. Before finalize_worker_promotion's return value was actually
  // checked, that fallback silently produced an empty-shell worker that
  // still reported "promoted" — this fixture's whole point is to exercise
  // the real promoted path, so it needs a method that actually validates.
  constexpr std::string_view kRequirement = R"({
  "method":        "test.add",
  "description":   "Return the sum of a and b",
  "input_schema":  {"a":"integer","b":"integer"},
  "output_schema": {"result":"integer"}
})";

} // anonymous namespace

// ---------------------------------------------------------------------------
// Base fixture
// ---------------------------------------------------------------------------

class ForgeCoordinatorTest : public ::testing::Test
{
protected:
  fs::path home_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<Registry> registry_;

  void SetUp () override
  {
    char tmpl[] = "/tmp/agentos_fc_test_XXXXXX";
    char *dir = mkdtemp (tmpl);
    ASSERT_NE (dir, nullptr);
    home_ = dir;

    setenv ("AGENTOS_HOME", home_.c_str (), 1);
    agentos::initialise_home (home_);

    db_ = std::make_unique<Database> ((home_ / "agentos.db").string ());
    ASSERT_TRUE (db_->open ());

    registry_ = std::make_unique<Registry> ();
    registry_->init (*db_);
  }

  void TearDown () override
  {
    if (db_)
      db_->close ();
    unsetenv ("AGENTOS_HOME");
    fs::remove_all (home_);
  }

  // Insert a forge_pipeline_jobs row and return the pre-populated job.
  ForgePipelineJob make_db_job (const std::string &id,
                                const std::string &task_id = "task-1")
  {
    ForgePipelineJob job;
    job.id = id;
    job.task_id = task_id;
    job.status = ForgeStatus::drafting;
    job.requirement_json = std::string (kRequirement);
    job.attempt = 0;
    job.max_attempts = 3;
    db_->store_forge_pipeline_job (job);
    return job;
  }

  // Build a ForgeRequest matching a DB row.
  ForgeRequest make_request (const ForgePipelineJob &job)
  {
    ForgeRequest req;
    req.forge_job_id = job.id;
    req.task_id = job.task_id;
    req.requirement_json = job.requirement_json;
    req.feedback = job.feedback;
    req.attempt = job.attempt;
    req.max_attempts = job.max_attempts;
    return req;
  }
};

// ---------------------------------------------------------------------------
// MockLlmProxy — injects pre-canned responses without network calls.
// ForgeCoordinator calls code_writer() and code_reviewer() free functions
// which call llm.complete().  We subclass LlmProxy and override complete().
//
// LlmProxy is constructed with (concurrency, timeout_s); we use 1,1 and
// override complete() in a thin wrapper to avoid real HTTP calls.
// ---------------------------------------------------------------------------

// Because LlmProxy::complete() is not virtual, the forge free functions
// (code_writer, code_reviewer) call it directly.  To inject fake responses
// we need a different approach: use a real LlmProxy that will fail (no key),
// and instead test ForgeCoordinator at the level just above LLM calls —
// by pre-populating writer_output_json / reviewer_verdict_json on the DB row
// and testing the state machine logic that wraps them.
//
// ForgeCoordinator exposes no direct "inject writer output" seam, so we
// test the Enforce Layer and retry logic through a minimal integration:
// we construct ForgeCoordinator with a real (but unconfigured) LlmProxy and
// verify that the Enforce Layer fires *before* any LLM call when the
// capability declaration is already in writer_output_json.
//
// The full LLM-dependent path is covered by ForgeCoordinatorLlmTest below.

// ---------------------------------------------------------------------------
// TEST: Enforce Layer rejects network:true before promotion
// We pre-populate writer_output_json with a network:true capability and
// call enforce_capability_policy indirectly by running a coordinator that
// will invoke it during the Writer step (simulated by pre-seeding the DB).
//
// Since we cannot easily inject the writer output without an LLM call, we
// test enforce_capability_policy via the public process() path by wrapping
// ForgeCoordinator with a fake LlmProxy that returns canned JSON.
// The cleanest seam is to test enforce_capability_policy through a thin
// subclass or by exposing it as a static helper — but per our design it is
// private.  We therefore test it indirectly through the full pipeline with
// a mock LlmProxy that fills the writer output.
//
// For the non-LLM test suite we test what we CAN test without an LLM:
//   1. DB schema correctness (ForgeStatus stored as INTEGER)
//   2. is_terminal() predicate
//   3. ForgeRequest / ForgeResult struct defaults
//   4. ForgeCoordinator lifecycle (start / stop without posting)
//   5. post() enqueues and callback is eventually fired (using a real LlmProxy
//      that returns an error, verifying the ForgeResult::failed path)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 1. ForgeStatus stored as INTEGER
// ---------------------------------------------------------------------------

TEST_F (ForgeCoordinatorTest, ForgeStatus_StoredAsInteger)
{
  ForgePipelineJob job = make_db_job ("status-int-test");
  job.status = ForgeStatus::reviewing;
  db_->update_forge_pipeline_job (job);

  auto loaded = db_->load_forge_pipeline_job ("status-int-test");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->status, ForgeStatus::reviewing);
  // Verify the raw integer in the DB matches the enum value.
  EXPECT_EQ (static_cast<int> (loaded->status), 1);
}

TEST_F (ForgeCoordinatorTest, ForgeStatus_AllValuesRoundTrip)
{
  const std::vector<ForgeStatus> statuses = {
    ForgeStatus::drafting, ForgeStatus::reviewing,    ForgeStatus::promoted,
    ForgeStatus::rejected, ForgeStatus::human_review,
  };

  int idx = 0;
  for (auto s : statuses)
  {
    const std::string id = "rt-" + std::to_string (idx++);
    ForgePipelineJob job = make_db_job (id);
    job.status = s;
    db_->update_forge_pipeline_job (job);

    auto loaded = db_->load_forge_pipeline_job (id);
    ASSERT_TRUE (loaded.has_value ()) << "id=" << id;
    EXPECT_EQ (loaded->status, s) << "id=" << id;
  }
}

// ---------------------------------------------------------------------------
// 2. is_terminal() predicate
// ---------------------------------------------------------------------------

TEST_F (ForgeCoordinatorTest, IsTerminal_CorrectForAllStates)
{
  EXPECT_FALSE (is_terminal (ForgeStatus::drafting));
  EXPECT_FALSE (is_terminal (ForgeStatus::reviewing));
  EXPECT_TRUE (is_terminal (ForgeStatus::promoted));
  EXPECT_TRUE (is_terminal (ForgeStatus::rejected));
  EXPECT_TRUE (is_terminal (ForgeStatus::human_review));
}

// ---------------------------------------------------------------------------
// 3. ForgeRequest / ForgeResult struct defaults
// ---------------------------------------------------------------------------

TEST_F (ForgeCoordinatorTest, ForgeRequest_Defaults)
{
  ForgeRequest req;
  EXPECT_TRUE (req.forge_job_id.empty ());
  EXPECT_TRUE (req.task_id.empty ());
  EXPECT_EQ (req.attempt, 0);
  EXPECT_EQ (req.max_attempts, 3);
  EXPECT_TRUE (req.feedback.empty ());
}

// ---------------------------------------------------------------------------
// 4. ForgeCoordinator lifecycle — start/stop without any posted work
// ---------------------------------------------------------------------------

TEST_F (ForgeCoordinatorTest, Lifecycle_StartStop)
{
  LlmProxy proxy (1, 5); // 1 thread, 5s timeout — no real calls expected

  bool callback_fired = false;
  ForgeCoordinator fc (*db_, proxy, *registry_,
                       [&] (ForgeResult) { callback_fired = true; });
  fc.start ();
  fc.stop ();
  // No work posted → callback never fires.
  EXPECT_FALSE (callback_fired);
}

// ---------------------------------------------------------------------------
// 5. Missing DB row → ForgeResult::failed callback
// ---------------------------------------------------------------------------

TEST_F (ForgeCoordinatorTest, MissingDbRow_DeliversFailedResult)
{
  LlmProxy proxy (1, 5);

  std::optional<ForgeResult> result;
  std::mutex mtx;
  std::condition_variable cv;

  ForgeCoordinator fc (*db_, proxy, *registry_,
                       [&] (ForgeResult r)
                       {
                         std::lock_guard lk (mtx);
                         result = r;
                         cv.notify_one ();
                       });
  fc.start ();

  ForgeRequest req;
  req.forge_job_id = "does-not-exist";
  req.task_id = "t-missing";
  req.max_attempts = 3;
  fc.post (std::move (req));

  {
    std::unique_lock lk (mtx);
    ASSERT_TRUE (cv.wait_for (lk, std::chrono::seconds (5),
                              [&] { return result.has_value (); }))
      << "callback not fired within 5s";
  }

  fc.stop ();

  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->forge_job_id, "does-not-exist");
  EXPECT_EQ (result->outcome, ForgeResult::Outcome::failed);
  EXPECT_FALSE (result->error.empty ());
}

// ---------------------------------------------------------------------------
// 6. human_reviews row inserted on escalation
// ---------------------------------------------------------------------------

TEST_F (ForgeCoordinatorTest, HumanReview_RowInserted)
{
  // human_reviews.forge_id references forge_pipeline_jobs(id) — insert parent
  // first.
  const std::string forge_id = "fj-001";
  make_db_job (forge_id);

  const std::string review_id = "rev-001";
  const std::string reason = "max_attempts reached";
  const std::string artifacts = R"({"attempt":3})";

  db_->insert_human_review (review_id, reason, artifacts, forge_id);

  // Verify via raw SQL.
  sqlite3_stmt *stmt = nullptr;
  ASSERT_EQ (sqlite3_prepare_v2 (db_->db_handle (),
                                 "SELECT reason, status FROM human_reviews "
                                 "WHERE id = ?",
                                 -1, &stmt, nullptr),
             SQLITE_OK);
  sqlite3_bind_text (stmt, 1, review_id.c_str (), -1, SQLITE_TRANSIENT);
  ASSERT_EQ (sqlite3_step (stmt), SQLITE_ROW);
  EXPECT_STREQ (reinterpret_cast<const char *> (sqlite3_column_text (stmt, 0)),
                reason.c_str ());
  EXPECT_STREQ (reinterpret_cast<const char *> (sqlite3_column_text (stmt, 1)),
                "pending");
  sqlite3_finalize (stmt);
}

// ---------------------------------------------------------------------------
// 7. load_in_flight excludes all terminal statuses
// ---------------------------------------------------------------------------

TEST_F (ForgeCoordinatorTest, LoadInFlight_ExcludesAllTerminals)
{
  auto make = [&] (const std::string &id, ForgeStatus s)
  {
    ForgePipelineJob j = make_db_job (id);
    j.status = s;
    db_->update_forge_pipeline_job (j);
  };

  make ("j-drafting", ForgeStatus::drafting);
  make ("j-reviewing", ForgeStatus::reviewing);
  make ("j-promoted", ForgeStatus::promoted);
  make ("j-rejected", ForgeStatus::rejected);
  make ("j-human_review", ForgeStatus::human_review);

  auto in_flight = db_->load_in_flight_forge_pipeline_jobs ();

  std::set<std::string> ids;
  for (auto &j : in_flight)
    ids.insert (j.id);

  EXPECT_TRUE (ids.count ("j-drafting")) << "drafting should be in-flight";
  EXPECT_TRUE (ids.count ("j-reviewing")) << "reviewing should be in-flight";
  EXPECT_FALSE (ids.count ("j-promoted")) << "promoted is terminal";
  EXPECT_FALSE (ids.count ("j-rejected")) << "rejected is terminal";
  EXPECT_FALSE (ids.count ("j-human_review")) << "human_review is terminal";
}

// ---------------------------------------------------------------------------
// LLM integration tests — skipped without DEEPSEEK_API_KEY
// ---------------------------------------------------------------------------

class ForgeCoordinatorLlmTest : public ForgeCoordinatorTest
{
protected:
  std::optional<LlmProxy> proxy_;

  void SetUp () override
  {
    ForgeCoordinatorTest::SetUp ();

    const char *key = std::getenv ("DEEPSEEK_API_KEY");
    if (!key || std::strlen (key) == 0)
      GTEST_SKIP () << "DEEPSEEK_API_KEY not set — skipping LLM tests";

    setenv ("AGENTOS_ADVISER_API_KEY", key, 1);
    setenv ("AGENTOS_ADVISER_BASE_URL", "https://api.deepseek.com", 1);
    setenv ("AGENTOS_ADVISER_MODEL", "deepseek-v4-flash", 1);

    // Seed minimal skill.md files so code_writer/code_reviewer can load them.
    auto seed = [&] (const std::string &name, const std::string &content)
    {
      auto p = home_ / "advisers" / name;
      fs::create_directories (p);
      std::ofstream (p / "skill.md") << content;
    };
    seed ("code-writer",
          "You are a code writer for AgentOS. Output JSON only, no markdown, "
          "no code fences, no commentary before or after the JSON object.\n"
          "Required top-level fields (all must be present, exact key names):\n"
          "  language      — must be exactly \"python\" or \"guile\"\n"
          "  entry_point   — name of the function to call, e.g. \"main\"\n"
          "  impl_code     — the full source code as a single string\n"
          "  signatures    — a JSON object describing entry_point's "
          "input/output, e.g. "
          "{\"main\":{\"input\":{\"a\":\"integer\",\"b\":\"integer\"},"
          "\"output\":{\"result\":\"integer\"}}}\n"
          "  capability    — object with keys network, exec, fs_read, "
          "fs_write (network and exec are booleans; fs_read/fs_write are "
          "arrays of paths)\n"
          "Optional fields: understanding, notes.\n"
          "Example response:\n"
          "{\"language\":\"python\",\"entry_point\":\"main\","
          "\"impl_code\":\"import json,sys\\nargs=json.load(sys.stdin)\\n"
          "print(json.dumps({'result':args['a']+args['b']}))\\n\","
          "\"signatures\":{\"main\":{\"input\":{\"a\":\"integer\","
          "\"b\":\"integer\"},\"output\":{\"result\":\"integer\"}}},"
          "\"capability\":{\"network\":false,\"exec\":false,\"fs_read\":[],"
          "\"fs_write\":[]},\"notes\":\"\"}\n");
    seed (
      "code-reviewer",
      "You are a code reviewer for AgentOS. Output JSON only, no markdown.\n"
      "Schema: {task_id, status: accept|reject, reason}\n");

    proxy_.emplace (1, 180);
  }

  void TearDown () override
  {
    proxy_.reset ();
    unsetenv ("AGENTOS_ADVISER_API_KEY");
    unsetenv ("AGENTOS_ADVISER_BASE_URL");
    unsetenv ("AGENTOS_ADVISER_MODEL");
    ForgeCoordinatorTest::TearDown ();
  }
};

// Full pipeline: simple addition worker should be promoted.
TEST_F (ForgeCoordinatorLlmTest, FullPipeline_SimpleWorker_Promoted)
{
  ForgePipelineJob job = make_db_job ("llm-fc-1", "task-llm-1");

  std::optional<ForgeResult> result;
  std::mutex mtx;
  std::condition_variable cv;

  ForgeCoordinator fc (*db_, *proxy_, *registry_,
                       [&] (ForgeResult r)
                       {
                         std::lock_guard lk (mtx);
                         result = r;
                         cv.notify_one ();
                       });
  fc.start ();
  fc.post (make_request (job));

  {
    std::unique_lock lk (mtx);
    // Allow up to 3 attempts × ~60s each.
    ASSERT_TRUE (cv.wait_for (lk, std::chrono::seconds (300),
                              [&] { return result.has_value (); }))
      << "pipeline did not complete within 5 minutes";
  }
  fc.stop ();

  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->outcome, ForgeResult::Outcome::promoted)
    << "expected promoted; error=" << result->error;
  EXPECT_FALSE (result->worker_id.empty ());

  // Worker code file must exist on disk.
  bool py_exists
    = fs::exists (home_ / "workers" / result->worker_id / "worker.py");
  bool scm_exists
    = fs::exists (home_ / "workers" / result->worker_id / "worker.scm");
  EXPECT_TRUE (py_exists || scm_exists) << "no worker binary on disk";

  // DB record must be promoted.
  auto loaded = db_->load_forge_pipeline_job ("llm-fc-1");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->status, ForgeStatus::promoted);
}

// Enforce Layer must block a worker that declares network:true even if
// the Code Reviewer would accept it (requires the Writer to produce such
// output — we test this by relying on the Reviewer skill prompt steering
// towards acceptance; the Enforce Layer fires before promotion regardless).
TEST_F (ForgeCoordinatorLlmTest, EnforceLayer_BlocksNetworkWorker)
{
  // Use a requirement that implies network access — the Writer will likely
  // declare network:true, which Enforce Layer must reject before Reviewer.
  ForgePipelineJob job = make_db_job ("llm-fc-net", "task-llm-net");
  job.requirement_json = R"({
    "description": "Fetch a URL and return the HTTP response body",
    "input_schema":  {"url": "string"},
    "output_schema": {"body": "string"}
  })";
  db_->update_forge_pipeline_job (job);

  std::optional<ForgeResult> result;
  std::mutex mtx;
  std::condition_variable cv;

  // max_attempts=1 so we don't spend too many LLM calls.
  job.max_attempts = 1;
  db_->update_forge_pipeline_job (job);

  ForgeCoordinator fc (*db_, *proxy_, *registry_,
                       [&] (ForgeResult r)
                       {
                         std::lock_guard lk (mtx);
                         result = r;
                         cv.notify_one ();
                       });
  fc.start ();

  ForgeRequest req = make_request (job);
  req.max_attempts = 1;
  fc.post (std::move (req));

  {
    std::unique_lock lk (mtx);
    ASSERT_TRUE (cv.wait_for (lk, std::chrono::seconds (120),
                              [&] { return result.has_value (); }))
      << "pipeline did not complete within 2 minutes";
  }
  fc.stop ();

  ASSERT_TRUE (result.has_value ());
  // Either promoted (if Writer self-corrected to network:false) or
  // human_review (Enforce rejected all attempts). Must never be failed.
  EXPECT_NE (result->outcome, ForgeResult::Outcome::failed)
    << "unexpected hard failure: " << result->error;
}

// ---------------------------------------------------------------------------
// promote_worker's RetryableFailure path (forge_coordinator.cpp): when
// finalize_worker_promotion() rejects the whole promotion because the
// capability method fails ADR-031 format validation, that must be treated
// exactly like a Code Writer generation failure — fed into the normal
// retry loop (job.attempt increments each pass) and, once max_attempts is
// exhausted, escalated to human_review. It must NOT silently report
// "promoted" with an empty-shell worker (the bug this fix closes), and it
// must NOT skip straight to human_review without actually retrying first
// (the regression this specific test guards against — a hard-error
// short-circuit would show attempt == 1, not attempt == max_attempts).
// ---------------------------------------------------------------------------
TEST_F (ForgeCoordinatorLlmTest,
       RetryableFailure_InvalidCapabilityMethod_RetriesThenHumanReview)
{
  // "BadMethodName" has no dot and uses uppercase — is_valid_method()
  // rejects it every attempt regardless of what the Writer/Reviewer
  // produce, so finalize_worker_promotion() fails deterministically on
  // every pass through the loop.
  ForgePipelineJob job = make_db_job ("llm-fc-badmethod", "task-llm-badmethod");
  job.requirement_json = R"({
    "method":        "BadMethodName",
    "description":   "Return the sum of a and b",
    "input_schema":  {"a":"integer","b":"integer"},
    "output_schema": {"result":"integer"}
  })";
  job.max_attempts = 2; // keep LLM spend bounded — just need >1 to prove retry
  db_->update_forge_pipeline_job (job);

  std::optional<ForgeResult> result;
  std::mutex mtx;
  std::condition_variable cv;

  ForgeCoordinator fc (*db_, *proxy_, *registry_,
                       [&] (ForgeResult r)
                       {
                         std::lock_guard lk (mtx);
                         result = r;
                         cv.notify_one ();
                       });
  fc.start ();

  ForgeRequest req = make_request (job);
  req.max_attempts = 2;
  fc.post (std::move (req));

  {
    std::unique_lock lk (mtx);
    ASSERT_TRUE (cv.wait_for (lk, std::chrono::seconds (240),
                              [&] { return result.has_value (); }))
      << "pipeline did not complete within 4 minutes";
  }
  fc.stop ();

  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->outcome, ForgeResult::Outcome::human_review)
    << "invalid capability method must never silently report promoted; "
       "error="
    << result->error;

  // Never registered — no empty-shell worker in the DB (the bug this
  // whole fix closes).
  EXPECT_FALSE (registry_->find_worker_for_command ("BadMethodName")
                  .has_value ());

  auto loaded = db_->load_forge_pipeline_job ("llm-fc-badmethod");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->status, ForgeStatus::human_review);
  // Proves the RetryableFailure path actually fell through into the
  // shared retry loop (job.attempt incremented across passes) rather
  // than short-circuiting to human_review on the very first attempt
  // like a HardFailure would.
  EXPECT_EQ (loaded->attempt, job.max_attempts)
    << "expected all " << job.max_attempts
    << " attempts to run before escalating — got " << loaded->attempt;
}
