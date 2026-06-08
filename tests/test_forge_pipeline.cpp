#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <set>

#include <rapidjson/document.h>

#include "agentos/database/database.h"
#include "agentos/forge/code_reviewer.h"
#include "agentos/forge/code_writer.h"
#include "agentos/forge_pipeline_job.h"
#include "agentos/home_init.h"
#include "agentos/registry.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Test fixture — isolated AGENTOS_HOME per test
// ---------------------------------------------------------------------------
class ForgePipelineTest : public ::testing::Test
{
protected:
  fs::path home_;
  std::unique_ptr<agentos::Database> db_;

  void SetUp () override
  {
    char tmpl[] = "/tmp/agentos_forge_test_XXXXXX";
    char *dir = mkdtemp (tmpl);
    ASSERT_NE (dir, nullptr);
    home_ = dir;

    setenv ("AGENTOS_HOME", home_.c_str (), 1);
    agentos::initialise_home (home_);

    db_ = std::make_unique<agentos::Database> (home_ / "agentos.db");
    ASSERT_TRUE (db_->open ());
  }

  void TearDown () override
  {
    if (db_)
      db_->close ();
    unsetenv ("AGENTOS_HOME");
    fs::remove_all (home_);
  }

  // Convenience: build a minimal valid ForgePipelineJob
  agentos::ForgePipelineJob make_job (std::string id,
                                      std::string status = "draft")
  {
    agentos::ForgePipelineJob j;
    j.id = std::move (id);
    j.task_id = "task-" + j.id;
    j.status = std::move (status);
    j.attempt = 0;
    j.max_attempts = 3;
    return j;
  }

  // Convenience: a minimal valid capability JSON for promotion tests
  static std::string make_cap_json (const std::string &agent_id)
  {
    return R"({
            "agent_id":")"
           + agent_id + R"(",
            "capabilities":[{
                "method":"example.run",
                "description":"does example work",
                "input_schema":{"x":"integer"},
                "output_schema":{"result":"string"}
            }],
            "requires":{"network":false,"fs_read":[],"fs_write":[],"exec":false},
            "provenance":{"forge_job_id":")"
           + agent_id + R"(","attempt":1}
        })";
  }
};

// ---------------------------------------------------------------------------
// ForgePipelineJob — struct defaults
// ---------------------------------------------------------------------------
TEST_F (ForgePipelineTest, StructDefaults)
{
  agentos::ForgePipelineJob j;
  EXPECT_TRUE (j.id.empty ());
  EXPECT_TRUE (j.task_id.empty ());
  EXPECT_TRUE (j.status.empty ());
  EXPECT_EQ (j.attempt, 0);
  EXPECT_EQ (j.max_attempts, 3);
  EXPECT_TRUE (j.last_code_path.empty ());
}

// ---------------------------------------------------------------------------
// Database — store and load round-trip
// ---------------------------------------------------------------------------
TEST_F (ForgePipelineTest, StoreAndLoad)
{
  auto job = make_job ("fj-1", "draft");
  job.requirement_json = R"({"desc":"sort a list"})";
  job.writer_output_json = R"({"code":"def main(): pass"})";
  job.feedback = "";
  job.attempt = 1;
  job.last_code_path = (home_ / "forge/fj-1/attempt_1.py").string ();

  db_->store_forge_pipeline_job (job);

  auto loaded = db_->load_forge_pipeline_job ("fj-1");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->id, "fj-1");
  EXPECT_EQ (loaded->task_id, "task-fj-1");
  EXPECT_EQ (loaded->status, "draft");
  EXPECT_EQ (loaded->requirement_json, R"({"desc":"sort a list"})");
  EXPECT_EQ (loaded->writer_output_json, R"({"code":"def main(): pass"})");
  EXPECT_EQ (loaded->attempt, 1);
  EXPECT_EQ (loaded->max_attempts, 3);
  EXPECT_EQ (loaded->last_code_path, job.last_code_path);
}

TEST_F (ForgePipelineTest, LoadNonExistentReturnsEmpty)
{
  auto result = db_->load_forge_pipeline_job ("does-not-exist");
  EXPECT_FALSE (result.has_value ());
}

// ---------------------------------------------------------------------------
// Database — update
// ---------------------------------------------------------------------------
TEST_F (ForgePipelineTest, UpdateStatus)
{
  auto job = make_job ("fj-upd", "draft");
  db_->store_forge_pipeline_job (job);

  job.status = "reviewing";
  job.writer_output_json = R"({"code":"def main(): return 42"})";
  job.attempt = 1;
  db_->update_forge_pipeline_job (job);

  auto loaded = db_->load_forge_pipeline_job ("fj-upd");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->status, "reviewing");
  EXPECT_EQ (loaded->writer_output_json, R"({"code":"def main(): return 42"})");
  EXPECT_EQ (loaded->attempt, 1);
}

TEST_F (ForgePipelineTest, UpdateFeedback)
{
  auto job = make_job ("fj-fb", "draft");
  db_->store_forge_pipeline_job (job);

  job.status = "draft";
  job.feedback = "output schema mismatch: expected {result: string}";
  job.attempt = 2;
  db_->update_forge_pipeline_job (job);

  auto loaded = db_->load_forge_pipeline_job ("fj-fb");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->feedback,
             "output schema mismatch: expected {result: string}");
  EXPECT_EQ (loaded->attempt, 2);
}

// ---------------------------------------------------------------------------
// Database — in-flight query
// Terminal states (promoted, failed) must be excluded; active states included
// ---------------------------------------------------------------------------
TEST_F (ForgePipelineTest, LoadInFlight_ExcludesTerminalStates)
{
  for (auto &[id, status] : std::vector<std::pair<std::string, std::string>>{
         {"j-draft", "draft"},
         {"j-reviewing", "reviewing"},
         {"j-promoted", "promoted"}, // terminal
       })
  {
    db_->store_forge_pipeline_job (make_job (id, status));
  }

  auto in_flight = db_->load_in_flight_forge_pipeline_jobs ();

  std::set<std::string> ids;
  for (auto &j : in_flight)
    ids.insert (j.id);

  EXPECT_TRUE (ids.count ("j-draft"));
  EXPECT_TRUE (ids.count ("j-reviewing"));
  EXPECT_FALSE (ids.count ("j-promoted")) << "promoted is a terminal state";
}

TEST_F (ForgePipelineTest, LoadInFlight_EmptyWhenAllTerminal)
{
  db_->store_forge_pipeline_job (make_job ("j1", "promoted"));
  db_->store_forge_pipeline_job (make_job ("j2", "rejected"));

  auto in_flight = db_->load_in_flight_forge_pipeline_jobs ();
  EXPECT_TRUE (in_flight.empty ());
}

// ---------------------------------------------------------------------------
// Registry — finalize_worker_promotion (ADR-019)
// Verifies: file written, DB status promoted, Registry worker count increased
// ---------------------------------------------------------------------------
TEST_F (ForgePipelineTest, FinalizeWorkerPromotion_WritesCodeFile)
{
  agentos::Registry reg (*db_);

  auto job = make_job ("forge-w1", "approved");
  job.attempt = 1;
  db_->store_forge_pipeline_job (job);

  reg.finalize_worker_promotion (job, "def main(): pass\n",
                                 make_cap_json ("forge-w1"), *db_);

  auto code_path = home_ / "workers" / "forge-w1" / "worker.py";
  ASSERT_TRUE (fs::exists (code_path)) << "worker code file not created";

  std::ifstream f (code_path);
  std::string content ((std::istreambuf_iterator<char> (f)),
                       std::istreambuf_iterator<char> ());
  EXPECT_EQ (content, "def main(): pass\n");
}

TEST_F (ForgePipelineTest, FinalizeWorkerPromotion_UpdatesJobStatusToPromoted)
{
  agentos::Registry reg (*db_);

  auto job = make_job ("forge-w2", "approved");
  db_->store_forge_pipeline_job (job);

  reg.finalize_worker_promotion (job, "def main(): pass\n",
                                 make_cap_json ("forge-w2"), *db_);

  auto updated = db_->load_forge_pipeline_job ("forge-w2");
  ASSERT_TRUE (updated.has_value ());
  EXPECT_EQ (updated->status, "promoted");
}

TEST_F (ForgePipelineTest, FinalizeWorkerPromotion_RegistersWorkerInRegistry)
{
  agentos::Registry reg (*db_);
  const size_t before = reg.worker_count ();

  auto job = make_job ("forge-w3", "approved");
  db_->store_forge_pipeline_job (job);

  reg.finalize_worker_promotion (job, "def main(): pass\n",
                                 make_cap_json ("forge-w3"), *db_);

  EXPECT_EQ (reg.worker_count (), before + 1);
}

TEST_F (ForgePipelineTest, FinalizeWorkerPromotion_WorkerIsDispatchable)
{
  agentos::Registry reg (*db_);

  auto job = make_job ("forge-w4", "approved");
  db_->store_forge_pipeline_job (job);

  reg.finalize_worker_promotion (job, "def main(): pass\n",
                                 make_cap_json ("forge-w4"), *db_);

  // The capability declared in make_cap_json is "example.run"
  auto found = reg.find_worker_for_command ("example.run");
  ASSERT_TRUE (found.has_value ()) << "promoted worker not findable by command";
}

// ---------------------------------------------------------------------------
// LLM integration tests — skipped unless keys are present
// These are NOT unit tests; keep them separate from CI.
// ---------------------------------------------------------------------------
class ForgeLlmTest : public ForgePipelineTest
{
protected:
  void SetUp () override
  {
    ForgePipelineTest::SetUp ();

    const char *key = std::getenv ("DEEPSEEK_API_KEY");
    if (!key || std::strlen (key) == 0)
      GTEST_SKIP ()
        << "DEEPSEEK_API_KEY not set — skipping LLM integration tests";

    setenv ("AGENTOS_ADVISER_API_KEY", key, 1);
    setenv ("AGENTOS_ADVISER_BASE_URL", "https://api.deepseek.com", 1);
    setenv ("AGENTOS_ADVISER_MODEL", "deepseek-chat", 1);
  }

  void TearDown () override
  {
    unsetenv ("AGENTOS_ADVISER_API_KEY");
    unsetenv ("AGENTOS_ADVISER_BASE_URL");
    unsetenv ("AGENTOS_ADVISER_MODEL");
    ForgePipelineTest::TearDown ();
  }
};

TEST_F (ForgeLlmTest, CodeWriterProducesValidJson)
{
  // Seed a minimal skill.md so Code Writer has a system prompt
  auto skill_path = home_ / "advisers/code-writer/skill.md";
  std::ofstream (skill_path) << "You are a code writer. Output JSON only.\n";
  setenv ("AGENTOS_ADVISER_SKILL_PATH", skill_path.c_str (), 1);

  std::string input = R"({
        "task_id": "llm-test-1",
        "forge_job_id": "forge-llm-1",
        "requirement": {
            "description": "Return the sum of two integers a and b",
            "input_schema":  {"a": "integer", "b": "integer"},
            "output_schema": {"result": "integer"}
        },
        "feedback": ""
    })";

  std::string out = agentos::forge::code_writer (input);

  rapidjson::Document doc;
  doc.Parse (out.c_str ());
  ASSERT_FALSE (doc.HasParseError ())
    << "code_writer returned invalid JSON: " << out;

  // Must have all required fields from ADR-019
  EXPECT_TRUE (doc.HasMember ("task_id"));
  EXPECT_TRUE (doc.HasMember ("understanding"));
  EXPECT_TRUE (doc.HasMember ("language"));
  EXPECT_TRUE (doc.HasMember ("entry_point"));
  EXPECT_TRUE (doc.HasMember ("code"));
  ASSERT_TRUE (doc.HasMember ("capability"));

  const auto &cap = doc["capability"];
  EXPECT_TRUE (cap.HasMember ("network"));
  EXPECT_TRUE (cap.HasMember ("fs_read"));
  EXPECT_TRUE (cap.HasMember ("fs_write"));
  EXPECT_TRUE (cap.HasMember ("exec"));

  // Sanity: language must be python or guile (bash forbidden, ADR-006)
  std::string lang = doc["language"].GetString ();
  EXPECT_TRUE (lang == "python" || lang == "guile")
    << "unexpected language: " << lang;
}

TEST_F (ForgeLlmTest, CodeReviewerAcceptsCorrectCode)
{
  // Guard: skip if API key not available (handled by ForgeLlmTest::SetUp)

  // Arrange: write skill.md
  auto skill_path = home_ / "advisers/code-reviewer/skill.md";
  std::ofstream (skill_path)
    << "You are a code reviewer for AgentOS.\n"
       "Review the submitted code for correctness against the requirement.\n"
       "You MUST respond with a JSON object only, no markdown, no "
       "explanation.\n"
       "Schema: {\"status\": \"accept\" | \"reject\", \"reason\": "
       "\"<string>\"}\n";
  setenv ("AGENTOS_ADVISER_SKILL_PATH", skill_path.c_str (), 1);

  // Arrange: create forge job directory so sandbox probe can write temp files
  auto forge_dir = home_ / "forge/forge-llm-accept";
  fs::create_directories (forge_dir);

  // Arrange: correct, executable Python worker
  // Reads JSON from stdin, writes JSON to stdout — matches input/output schema
  std::string good_code
    = "import json, sys\n"
      "args = json.load(sys.stdin)\n"
      "print(json.dumps({\"result\": args[\"a\"] + args[\"b\"]}))\n";

  std::string input = R"({
    "task_id":      "llm-accept-1",
    "forge_job_id": "forge-llm-accept",
    "requirement": {
      "description":   "Return the integer sum of inputs a and b",
      "input_schema":  {"a": "integer", "b": "integer"},
      "output_schema": {"result": "integer"}
    },
    "writer_output": {
      "task_id":      "llm-accept-1",
      "understanding":"Read two integers from stdin JSON and return their sum as JSON",
      "language":     "python",
      "entry_point":  "main",
      "code":         "import json, sys\nargs = json.load(sys.stdin)\nprint(json.dumps({\"result\": args[\"a\"] + args[\"b\"]}))\n",
      "capability":   {"network": false, "fs_read": [], "fs_write": [], "exec": false},
      "notes":        ""
    }
  })";

  // Act
  std::string out = agentos::forge::code_reviewer (input);

  // Assert: must be valid JSON
  rapidjson::Document doc;
  doc.Parse (out.c_str ());
  ASSERT_FALSE (doc.HasParseError ())
    << "code_reviewer returned invalid JSON: " << out;

  // Assert: required fields present with correct types
  ASSERT_TRUE (doc.HasMember ("task_id")) << out;
  ASSERT_TRUE (doc.HasMember ("status")) << out;
  ASSERT_TRUE (doc.HasMember ("reason")) << out;
  ASSERT_TRUE (doc["task_id"].IsString ());
  ASSERT_TRUE (doc["status"].IsString ());
  ASSERT_TRUE (doc["reason"].IsString ());

  // Assert: task_id echoed correctly
  EXPECT_STREQ (doc["task_id"].GetString (), "llm-accept-1");

  // Assert: status is one of the two legal values
  std::string status = doc["status"].GetString ();
  EXPECT_TRUE (status == "accept" || status == "reject")
    << "unexpected status: " << status;

  // Assert: reason is non-empty regardless of verdict
  EXPECT_GT (std::strlen (doc["reason"].GetString ()), 0u);

  // Assert: correct code + correct capability should be accepted
  // This is a soft expectation — LLM may occasionally disagree, but
  // this is the expected happy-path outcome
  EXPECT_EQ (status, "accept") << "Expected accept for correct code; reason: "
                               << doc["reason"].GetString ();
}

TEST_F (ForgeLlmTest, CodeReviewerRejectsWrongOutput)
{
  auto skill_path = home_ / "advisers/code-reviewer/skill.md";
  std::ofstream (skill_path)
    << "You are a code reviewer for AgentOS.\n"
       "Review the submitted code for correctness against the requirement.\n"
       "You MUST respond with a JSON object only, no markdown, no "
       "explanation.\n"
       "Schema: {\"status\": \"accept\" | \"reject\", \"reason\": "
       "\"<string>\"}\n";
  setenv ("AGENTOS_ADVISER_SKILL_PATH", skill_path.c_str (), 1);

  auto forge_dir = home_ / "forge/forge-llm-reject";
  fs::create_directories (forge_dir);

  // Wrong code: returns difference instead of sum
  std::string input = R"({
    "task_id":      "llm-reject-1",
    "forge_job_id": "forge-llm-reject",
    "requirement": {
      "description":   "Return the integer sum of inputs a and b",
      "input_schema":  {"a": "integer", "b": "integer"},
      "output_schema": {"result": "integer"}
    },
    "writer_output": {
      "task_id":      "llm-reject-1",
      "understanding":"Subtract b from a and return the result",
      "language":     "python",
      "entry_point":  "main",
      "code":         "import json, sys\nargs = json.load(sys.stdin)\nprint(json.dumps({\"result\": args[\"a\"] - args[\"b\"]}))\n",
      "capability":   {"network": false, "fs_read": [], "fs_write": [], "exec": false},
      "notes":        ""
    }
  })";

  std::string out = agentos::forge::code_reviewer (input);

  rapidjson::Document doc;
  doc.Parse (out.c_str ());
  ASSERT_FALSE (doc.HasParseError ()) << out;
  ASSERT_TRUE (doc.HasMember ("status")) << out;
  ASSERT_TRUE (doc.HasMember ("reason")) << out;

  // understanding says "subtract" but requirement says "sum" —
  // Reviewer should catch this
  EXPECT_STREQ (doc["status"].GetString (), "reject")
    << "Expected reject for wrong logic; reason: "
    << doc["reason"].GetString ();
  EXPECT_GT (std::strlen (doc["reason"].GetString ()), 0u);
}

TEST_F (ForgeLlmTest, CodeReviewerRejectsPolicyViolation_NetworkTrue)
{
  // This test does NOT need an LLM call — Enforce Layer pre-check fires first
  // Still lives in ForgeLlmTest for fixture convenience, but will run even
  // without a real API key if we remove the skip guard... however since
  // ForgeLlmTest::SetUp skips without a key we accept that limitation here.

  auto skill_path = home_ / "advisers/code-reviewer/skill.md";
  std::ofstream (skill_path) << "You are a code reviewer.\n";
  setenv ("AGENTOS_ADVISER_SKILL_PATH", skill_path.c_str (), 1);

  auto forge_dir = home_ / "forge/forge-llm-policy";
  fs::create_directories (forge_dir);

  // capability declares network: true — immediate terminal rejection
  std::string input = R"({
    "task_id":      "llm-policy-1",
    "forge_job_id": "forge-llm-policy",
    "requirement": {
      "description":   "Fetch a URL and return the response body",
      "input_schema":  {"url": "string"},
      "output_schema": {"body": "string"}
    },
    "writer_output": {
      "task_id":      "llm-policy-1",
      "understanding":"Fetch URL over HTTP",
      "language":     "python",
      "entry_point":  "main",
      "code":         "import json, sys, urllib.request\nargs = json.load(sys.stdin)\nprint(json.dumps({\"body\": urllib.request.urlopen(args[\"url\"]).read().decode()}))\n",
      "capability":   {"network": true, "fs_read": [], "fs_write": [], "exec": false},
      "notes":        ""
    }
  })";

  std::string out = agentos::forge::code_reviewer (input);

  rapidjson::Document doc;
  doc.Parse (out.c_str ());
  ASSERT_FALSE (doc.HasParseError ()) << out;
  ASSERT_TRUE (doc.HasMember ("status")) << out;
  ASSERT_TRUE (doc.HasMember ("reason")) << out;

  // Enforce Layer must reject before any sandbox or LLM work
  EXPECT_STREQ (doc["status"].GetString (), "reject") << out;
  EXPECT_GT (std::strlen (doc["reason"].GetString ()), 0u);
}

TEST_F (ForgeLlmTest, CodeReviewerReturnsMissingFields_IsError)
{
  // Malformed input — missing writer_output entirely
  // code_reviewer must return a parseable JSON error, not crash or return
  // garbage

  auto skill_path = home_ / "advisers/code-reviewer/skill.md";
  std::ofstream (skill_path) << "You are a code reviewer.\n";
  setenv ("AGENTOS_ADVISER_SKILL_PATH", skill_path.c_str (), 1);

  std::string input = R"({
    "task_id": "llm-bad-input",
    "forge_job_id": "forge-llm-bad"
  })";

  std::string out = agentos::forge::code_reviewer (input);

  // Must return valid JSON — never crash or return empty string
  rapidjson::Document doc;
  doc.Parse (out.c_str ());
  ASSERT_FALSE (doc.HasParseError ())
    << "code_reviewer must return valid JSON even on bad input; got: " << out;

  // Either an error structure or a reject verdict — both acceptable,
  // but it must be parseable and non-empty
  EXPECT_FALSE (out.empty ());
}
