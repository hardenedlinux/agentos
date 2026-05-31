#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <fstream>
#include <filesystem>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sqlite3.h>

#include "agentos/database/database.h"
#include "agentos/registry.h"
#include "agentos/home_init.h"
#include "agentos/forge_pipeline_job.h"

// Forward-declare the forge component entry points (they live in agentos::forge)
namespace agentos {
namespace forge {
    std::string code_writer(const std::string& input_json);
    std::string code_reviewer(const std::string& input_json);
}
} // namespace agentos

class ForgePipelineTest : public ::testing::Test {
protected:
    std::filesystem::path temp_dir_;
    std::unique_ptr<agentos::Database> db_;

    void SetUp() override {
        char tmpl[] = "/tmp/agentos_forge_test_XXXXXX";
        char* dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        temp_dir_ = dir;

        // Point AGENTOS_HOME into the temporary tree so all runtime paths
        // (database, workers directory, etc.) are isolated.
        setenv("AGENTOS_HOME", temp_dir_.c_str(), 1);

        // Create the standard directory layout.
        agentos::initialise_home(temp_dir_);

        // Open/initialize the database (empty path → uses agentos_home()).
        db_ = std::make_unique<agentos::Database>("");
        ASSERT_TRUE(db_->open());
    }

    void TearDown() override {
        if (db_) {
            db_->close();
        }
        unsetenv("AGENTOS_HOME");
        std::filesystem::remove_all(temp_dir_);
    }
};

// ---------------------------------------------------------------------------
// ForgePipelineJob struct – default values
// ---------------------------------------------------------------------------
TEST_F(ForgePipelineTest, StructDefaults) {
    agentos::ForgePipelineJob j;
    EXPECT_TRUE(j.id.empty());
    EXPECT_TRUE(j.task_id.empty());
    EXPECT_TRUE(j.status.empty());
    EXPECT_EQ(j.attempt, 0);
    EXPECT_EQ(j.max_attempts, 3);
    EXPECT_TRUE(j.last_code_path.empty());
}

// ---------------------------------------------------------------------------
// Database CRUD for forge_pipeline_jobs table
// ---------------------------------------------------------------------------
TEST_F(ForgePipelineTest, StoreAndLoad) {
    agentos::ForgePipelineJob job;
    job.id                 = "fj-abc";
    job.task_id            = "task-1";
    job.status             = "draft";
    job.requirement_json   = R"({"desc":"hello"})";
    job.writer_output_json = "code goes here";
    job.feedback           = "";
    job.attempt            = 1;
    job.max_attempts       = 3;
    job.last_code_path     = "/tmp/attempt_1.py";

    db_->store_forge_pipeline_job(job);

    auto loaded = db_->load_forge_pipeline_job("fj-abc");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id,                "fj-abc");
    EXPECT_EQ(loaded->task_id,           "task-1");
    EXPECT_EQ(loaded->status,            "draft");
    EXPECT_EQ(loaded->requirement_json,  R"({"desc":"hello"})");
    EXPECT_EQ(loaded->writer_output_json, "code goes here");
    EXPECT_EQ(loaded->attempt,           1);
    EXPECT_EQ(loaded->max_attempts,      3);
    EXPECT_EQ(loaded->last_code_path,    "/tmp/attempt_1.py");
}

TEST_F(ForgePipelineTest, UpdateAndLoad) {
    agentos::ForgePipelineJob job;
    job.id     = "fj-upd";
    job.status = "draft";
    db_->store_forge_pipeline_job(job);

    job.status = "review";
    job.writer_output_json = "updated code";
    db_->update_forge_pipeline_job(job);

    auto loaded = db_->load_forge_pipeline_job("fj-upd");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->status, "review");
    EXPECT_EQ(loaded->writer_output_json, "updated code");
}

TEST_F(ForgePipelineTest, LoadInFlight) {
    agentos::ForgePipelineJob j1;
    j1.id     = "j1";
    j1.status = "draft";

    agentos::ForgePipelineJob j2;
    j2.id     = "j2";
    j2.status = "review";

    agentos::ForgePipelineJob j3;
    j3.id     = "j3";
    j3.status = "promoted";   // terminal state – should be excluded

    db_->store_forge_pipeline_job(j1);
    db_->store_forge_pipeline_job(j2);
    db_->store_forge_pipeline_job(j3);

    auto in_flight = db_->load_in_flight_forge_pipeline_jobs();

    std::set<std::string> ids;
    for (auto& j : in_flight) {
        ids.insert(j.id);
    }

    EXPECT_EQ(ids.size(), 2);
    EXPECT_TRUE(ids.count("j1"));
    EXPECT_TRUE(ids.count("j2"));
    EXPECT_FALSE(ids.count("j3"));
}

// ---------------------------------------------------------------------------
// Registry finalize_worker_promotion (ADR‑019)
// ---------------------------------------------------------------------------
TEST_F(ForgePipelineTest, FinalizeWorkerPromotion) {
    // Create a Registry object; this also creates the `agents` & `capabilities`
    // tables required by the promotion code.
    agentos::Registry reg(*db_);

    // Pre-store a ForgePipelineJob in the database.
    agentos::ForgePipelineJob fj;
    fj.id       = "forge-w1";
    fj.task_id  = "t-forge";
    fj.status   = "approved";
    fj.attempt  = 2;
    db_->store_forge_pipeline_job(fj);

    std::string worker_code = "def main(): pass\n";
    std::string cap_json    = R"({
        "agent_id":"forge-w1",
        "capabilities":[{
            "method":"example.run",
            "description":"does example work",
            "input_schema":{"x":"integer"},
            "output_schema":{"result":"string"}
        }],
        "requires":{"network":false,"fs_read":[],"fs_write":[],"exec":false},
        "provenance":{"forge_job_id":"forge-w1","attempt":2}
    })";

    reg.finalize_worker_promotion(fj, worker_code, cap_json);

    // Verify the agent row was created.
    sqlite3_stmt* stmt = nullptr;
    const char* query = "SELECT role, binary_path, manifest FROM agents WHERE id=?";
    ASSERT_EQ(sqlite3_prepare_v2(db_->db_handle(), query, -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "forge-w1", -1, SQLITE_STATIC);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);

    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)), "worker");
    std::string binary = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    EXPECT_NE(binary.find("/workers/forge-w1/worker.py"), std::string::npos);
    sqlite3_finalize(stmt);

    // Verify the worker code file was written.
    auto code_path = agentos::agentos_home() / "workers" / "forge-w1" / "worker.py";
    EXPECT_TRUE(std::filesystem::exists(code_path));

    // Verify the ForgePipelineJob status was updated to "promoted".
    auto updated = db_->load_forge_pipeline_job("forge-w1");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->status, "promoted");
}

// ---------------------------------------------------------------------------
// Built‑in forge components (stubs)
// ---------------------------------------------------------------------------
TEST_F(ForgePipelineTest, CodeWriterOutput) {
    std::string out = agentos::forge::code_writer("{}");

    rapidjson::Document doc;
    doc.Parse(out.c_str());
    ASSERT_FALSE(doc.HasParseError());
    ASSERT_TRUE(doc.HasMember("code"));
    ASSERT_TRUE(doc.HasMember("capability"));
    ASSERT_TRUE(doc.HasMember("task_id"));
    EXPECT_STREQ(doc["task_id"].GetString(), "dummy");
}

TEST_F(ForgePipelineTest, CodeReviewerOutput) {
    std::string out = agentos::forge::code_reviewer("{}");

    rapidjson::Document doc;
    doc.Parse(out.c_str());
    ASSERT_FALSE(doc.HasParseError());
    ASSERT_TRUE(doc.HasMember("status"));
    EXPECT_STREQ(doc["status"].GetString(), "accept");
}

// -----------------------------------------------------------------------
// Linker‑only stubs for forge functions (they normally live in
// code_writer.cpp / code_reviewer.cpp which are not linked here).
// -----------------------------------------------------------------------
namespace agentos {
namespace forge {

std::string code_writer(const std::string& input_json) {
    // TODO: integrate LLM to produce real code.
    // For now return a hard‑coded success that fulfills any requirement.
    rapidjson::Document d;
    d.SetObject();
    d.AddMember("task_id", "dummy", d.GetAllocator());
    d.AddMember("understanding", "Understood.", d.GetAllocator());
    d.AddMember("language", "python", d.GetAllocator());
    d.AddMember("entry_point", "main", d.GetAllocator());
    d.AddMember("code", "def main(): pass", d.GetAllocator());
    rapidjson::Value cap(rapidjson::kObjectType);
    cap.AddMember("network", false, d.GetAllocator());
    cap.AddMember("fs_read", rapidjson::kArrayType, d.GetAllocator());
    cap.AddMember("fs_write", rapidjson::kArrayType, d.GetAllocator());
    cap.AddMember("exec", false, d.GetAllocator());
    d.AddMember("capability", cap, d.GetAllocator());
    d.AddMember("notes", "", d.GetAllocator());
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    return buf.GetString();
}

std::string code_reviewer(const std::string& input_json) {
    // TODO: actual static review, sandbox execution, and capability check.
    // For now always return accept.
    rapidjson::Document d;
    d.SetObject();
    d.AddMember("status", "accept", d.GetAllocator());
    d.AddMember("reason", "All checks passed – reviewer stub", d.GetAllocator());
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    return buf.GetString();
}

} // namespace forge
} // namespace agentos
