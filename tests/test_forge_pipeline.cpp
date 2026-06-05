#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <set>

#include <rapidjson/document.h>

#include "agentos/database/database.h"
#include "agentos/forge_pipeline_job.h"
#include "agentos/home_init.h"
#include "agentos/registry.h"

namespace agentos::forge
{
  std::string code_writer (const std::string &input_json);
  std::string code_reviewer (const std::string &input_json);
} // namespace agentos::forge

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
  db_->store_forge_pipeline_job (make_job ("j2", "failed"));

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
                                 make_cap_json ("forge-w1"));

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
                                 make_cap_json ("forge-w2"));

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
                                 make_cap_json ("forge-w3"));

  EXPECT_EQ (reg.worker_count (), before + 1);
}

TEST_F (ForgePipelineTest, FinalizeWorkerPromotion_WorkerIsDispatchable)
{
  agentos::Registry reg (*db_);

  auto job = make_job ("forge-w4", "approved");
  db_->store_forge_pipeline_job (job);

  reg.finalize_worker_promotion (job, "def main(): pass\n",
                                 make_cap_json ("forge-w4"));

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

TEST_F (ForgeLlmTest, CodeReviewerReturnsValidVerdict)
{
  auto skill_path = home_ / "advisers/code-reviewer/skill.md";
  std::ofstream (skill_path) << "You are a code reviewer. Output JSON only.\n";
  setenv ("AGENTOS_ADVISER_SKILL_PATH", skill_path.c_str (), 1);

  std::string input = R"({
        "task_id": "llm-test-2",
        "forge_job_id": "forge-llm-2",
        "requirement": {
            "description": "Return the sum of two integers a and b",
            "input_schema":  {"a": "integer", "b": "integer"},
            "output_schema": {"result": "integer"}
        },
        "writer_output": {
            "task_id": "llm-test-2",
            "understanding": "Sum two integers",
            "language": "python",
            "entry_point": "main",
            "code": "def main(a, b):\n    return {\"result\": a + b}\n",
            "capability": {"network": false, "fs_read": [], "fs_write": [], "exec": false},
            "notes": ""
        }
    })";

  std::string out = agentos::forge::code_reviewer (input);

  rapidjson::Document doc;
  doc.Parse (out.c_str ());
  ASSERT_FALSE (doc.HasParseError ())
    << "code_reviewer returned invalid JSON: " << out;

  ASSERT_TRUE (doc.HasMember ("task_id"));
  ASSERT_TRUE (doc.HasMember ("status"));
  ASSERT_TRUE (doc.HasMember ("reason"));

  std::string status = doc["status"].GetString ();
  EXPECT_TRUE (status == "accept" || status == "reject")
    << "unexpected status: " << status;

  // reason must be non-empty regardless of verdict
  EXPECT_GT (std::strlen (doc["reason"].GetString ()), 0u);
}
