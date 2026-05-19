#include <gtest/gtest.h>
#include "database/database.hpp"
#include <filesystem>

using namespace agentos;

class DatabaseTest : public ::testing::Test {
protected:
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        db_path = "/tmp/agentos_test_db_" + std::to_string(getpid()) + ".db";
        db = std::make_unique<Database>(db_path);
        ASSERT_TRUE(db->open());
    }

    void TearDown() override {
        db->close();
        std::filesystem::remove(db_path);
    }
};

TEST_F(DatabaseTest, OpenAndClose) {
    EXPECT_TRUE(db->is_open());
    db->close();
    EXPECT_FALSE(db->is_open());
}

TEST_F(DatabaseTest, StoreAndLoadJob) {
    Task task;
    task.id = "job1";
    task.goal = "test goal";
    task.input_json = R"({"key":"value"})";
    db->store_job(task);
    db->update_job_phase("job1", "planning");
    std::string plan_json = R"({"steps":[]})";
    db->update_job_plan("job1", plan_json);
    std::string loaded = db->load_plan_json("job1");
    EXPECT_EQ(loaded, plan_json);
}

TEST_F(DatabaseTest, StoreTask) {
    Task task;
    task.id = "job1";
    task.goal = "test";
    db->store_job(task);
    PlanStep step;
    step.id = "step1";
    step.command = "cmd1";
    step.args["arg1"] = "val1";
    db->store_task("job1", step);
}

TEST_F(DatabaseTest, ResumeInFlight) {
    Task task;
    task.id = "job1";
    task.goal = "test";
    db->store_job(task);
    std::string plan_json = R"({"steps":[]})";
    db->update_job_plan("job1", plan_json);
    auto jobs = db->resume_in_flight();
    ASSERT_EQ(jobs.size(), 1);
    EXPECT_EQ(jobs[0].job_id, "job1");
    EXPECT_EQ(jobs[0].plan_json, plan_json);
}

TEST_F(DatabaseTest, ResumeInFlightNone) {
    auto jobs = db->resume_in_flight();
    EXPECT_TRUE(jobs.empty());
}
#include <gtest/gtest.h>
#include "database/database.hpp"
#include <string>

using namespace agentos;

// Helper to create a minimal Task for testing
static Task make_task(const std::string& id, const std::string& input) {
    Task t;
    t.id = id;
    t.input_json = input;
    return t;
}

// Helper to create a minimal PlanStep
static PlanStep make_step(const std::string& id, const std::string& command) {
    PlanStep s;
    s.id = id;
    s.command = command;
    return s;
}

// ---------------------------------------------------------------------------
// Database tests
// ---------------------------------------------------------------------------

TEST(DatabaseTest, OpenClose) {
    Database db(":memory:");
    EXPECT_TRUE(db.open());
    EXPECT_TRUE(db.is_open());
    db.close();
    EXPECT_FALSE(db.is_open());
}

TEST(DatabaseTest, StoreAndLoadJob) {
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    Task task = make_task("job-001", R"({"goal":"test"})");
    db.store_job(task);

    // After storing, the job should be retrievable via load_plan_json (empty plan)
    std::string plan = db.load_plan_json("job-001");
    EXPECT_TRUE(plan.empty());  // no plan stored yet

    db.close();
}

TEST(DatabaseTest, UpdateJobPhase) {
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    Task task = make_task("job-002", R"({"goal":"phase-test"})");
    db.store_job(task);

    db.update_job_phase("job-002", "executing");

    // We cannot directly read the phase back via the current API,
    // but we can verify that resume_in_flight includes the job
    // (since phase is not 'done' or 'failed')
    auto in_flight = db.resume_in_flight();
    bool found = false;
    for (const auto& j : in_flight) {
        if (j.job_id == "job-002") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "job-002 should be in-flight after phase update";

    db.close();
}

TEST(DatabaseTest, UpdateJobPlan) {
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    Task task = make_task("job-003", R"({"goal":"plan-test"})");
    db.store_job(task);

    const std::string plan_json = R"({"steps":[{"id":"s1","command":"echo"}]})";
    db.update_job_plan("job-003", plan_json);

    std::string loaded = db.load_plan_json("job-003");
    EXPECT_EQ(loaded, plan_json);

    db.close();
}

TEST(DatabaseTest, StoreTask) {
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    Task task = make_task("job-004", R"({"goal":"task-test"})");
    db.store_job(task);

    PlanStep step = make_step("step-1", "echo");
    step.args["message"] = "hello";
    db.store_task("job-004", step);

    // No direct retrieval API for tasks, but the operation should not crash
    db.close();
}

TEST(DatabaseTest, ResumeInFlight) {
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    // Store two jobs, one with a plan, one without
    Task task1 = make_task("job-a", R"({"goal":"a"})");
    db.store_job(task1);
    db.update_job_plan("job-a", R"({"steps":[]})");

    Task task2 = make_task("job-b", R"({"goal":"b"})");
    db.store_job(task2);
    // job-b has no plan

    auto in_flight = db.resume_in_flight();
    EXPECT_EQ(in_flight.size(), 2u);

    // job-a should have its plan
    for (const auto& j : in_flight) {
        if (j.job_id == "job-a") {
            EXPECT_FALSE(j.plan_json.empty());
        }
    }

    db.close();
}

TEST(DatabaseTest, ResumeInFlightSkipsDone) {
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    Task task = make_task("job-done", R"({"goal":"done"})");
    db.store_job(task);
    db.update_job_phase("job-done", "done");

    auto in_flight = db.resume_in_flight();
    for (const auto& j : in_flight) {
        EXPECT_NE(j.job_id, "job-done");
    }

    db.close();
}
