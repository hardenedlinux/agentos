#include <gtest/gtest.h>
#include <sqlite3.h>
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <thread>

#include "forge_job.hpp"
#include "forge_database.hpp"
#include "forge_state_machine.hpp"
#include "forge_manager.hpp"
#include "database.hpp"
#include "agentos/registry.h"
#include "agentos/dispatcher.h"
#include "agentos/orchestrator.h"
#include "agentos/obs_bus.h"

using namespace agentos;

// -----------------------------------------------------------------------
// Mock Database that provides a real sqlite3 handle for testing
// -----------------------------------------------------------------------
class MockDatabase : public Database {
public:
    MockDatabase() : Database(":memory:") {
        open();
    }
    ~MockDatabase() {
        close();
    }
};

// -----------------------------------------------------------------------
// Test ForgeJob struct
// -----------------------------------------------------------------------
TEST(ForgeJobTest, DefaultValues) {
    ForgeJob job;
    EXPECT_EQ(job.attempt, 0);
    EXPECT_EQ(job.max_attempts, 3);
    EXPECT_TRUE(job.phase.empty());
    EXPECT_TRUE(job.last_code.empty());
    EXPECT_TRUE(job.last_feedback.empty());
    EXPECT_EQ(job.created_at, 0);
    EXPECT_EQ(job.updated_at, 0);
}

TEST(ForgeJobTest, CustomValues) {
    ForgeJob job;
    job.id = "test_id";
    job.method = "test_method";
    job.requirement = "test_requirement";
    job.attempt = 2;
    job.max_attempts = 5;
    job.phase = "Drafting";
    job.last_code = "code";
    job.last_feedback = "feedback";
    job.created_at = 1000;
    job.updated_at = 2000;

    EXPECT_EQ(job.id, "test_id");
    EXPECT_EQ(job.method, "test_method");
    EXPECT_EQ(job.requirement, "test_requirement");
    EXPECT_EQ(job.attempt, 2);
    EXPECT_EQ(job.max_attempts, 5);
    EXPECT_EQ(job.phase, "Drafting");
    EXPECT_EQ(job.last_code, "code");
    EXPECT_EQ(job.last_feedback, "feedback");
    EXPECT_EQ(job.created_at, 1000);
    EXPECT_EQ(job.updated_at, 2000);
}

// -----------------------------------------------------------------------
// Test ForgeDatabase
// -----------------------------------------------------------------------
class ForgeDatabaseTest : public ::testing::Test {
protected:
    MockDatabase mockDb;
    ForgeDatabase forgeDb{mockDb};

    void SetUp() override {
        forgeDb.create_tables();
    }
};

TEST_F(ForgeDatabaseTest, CreateTables) {
    // Should not throw
    forgeDb.create_tables();
}

TEST_F(ForgeDatabaseTest, InsertAndGetJob) {
    ForgeJob job;
    job.id = "job1";
    job.method = "method1";
    job.requirement = "req1";
    job.attempt = 0;
    job.max_attempts = 3;
    job.phase = "Drafting";
    job.last_code = "";
    job.last_feedback = "";
    job.created_at = 100;
    job.updated_at = 100;

    forgeDb.insert_job(job);

    auto opt = forgeDb.get_job("job1");
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->id, "job1");
    EXPECT_EQ(opt->method, "method1");
    EXPECT_EQ(opt->requirement, "req1");
    EXPECT_EQ(opt->attempt, 0);
    EXPECT_EQ(opt->max_attempts, 3);
    EXPECT_EQ(opt->phase, "Drafting");
    EXPECT_EQ(opt->created_at, 100);
    EXPECT_EQ(opt->updated_at, 100);
}

TEST_F(ForgeDatabaseTest, UpdateJob) {
    ForgeJob job;
    job.id = "job2";
    job.method = "method2";
    job.requirement = "req2";
    job.attempt = 0;
    job.max_attempts = 3;
    job.phase = "Drafting";
    job.created_at = 200;
    job.updated_at = 200;
    forgeDb.insert_job(job);

    job.phase = "Reviewing";
    job.attempt = 1;
    job.updated_at = 300;
    forgeDb.update_job(job);

    auto opt = forgeDb.get_job("job2");
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->phase, "Reviewing");
    EXPECT_EQ(opt->attempt, 1);
    EXPECT_EQ(opt->updated_at, 300);
}

TEST_F(ForgeDatabaseTest, GetJobsByPhase) {
    ForgeJob job1;
    job1.id = "j1";
    job1.method = "m1";
    job1.requirement = "r1";
    job1.phase = "Drafting";
    job1.created_at = 1;
    job1.updated_at = 1;
    forgeDb.insert_job(job1);

    ForgeJob job2;
    job2.id = "j2";
    job2.method = "m2";
    job2.requirement = "r2";
    job2.phase = "HumanReview";
    job2.created_at = 2;
    job2.updated_at = 2;
    forgeDb.insert_job(job2);

    auto draftingJobs = forgeDb.get_jobs_by_phase("Drafting");
    EXPECT_EQ(draftingJobs.size(), 1);
    EXPECT_EQ(draftingJobs[0].id, "j1");

    auto humanReviewJobs = forgeDb.get_jobs_by_phase("HumanReview");
    EXPECT_EQ(humanReviewJobs.size(), 1);
    EXPECT_EQ(humanReviewJobs[0].id, "j2");

    auto approvedJobs = forgeDb.get_jobs_by_phase("Approved");
    EXPECT_TRUE(approvedJobs.empty());
}

TEST_F(ForgeDatabaseTest, GetAllJobs) {
    ForgeJob job1;
    job1.id = "a1";
    job1.method = "m1";
    job1.requirement = "r1";
    job1.phase = "Drafting";
    job1.created_at = 1;
    job1.updated_at = 1;
    forgeDb.insert_job(job1);

    ForgeJob job2;
    job2.id = "a2";
    job2.method = "m2";
    job2.requirement = "r2";
    job2.phase = "HumanReview";
    job2.created_at = 2;
    job2.updated_at = 2;
    forgeDb.insert_job(job2);

    auto all = forgeDb.get_all_jobs();
    EXPECT_EQ(all.size(), 2);
}

// -----------------------------------------------------------------------
// Test ForgeStateMachine
// -----------------------------------------------------------------------
TEST(ForgeStateMachineTest, DraftingToReviewing) {
    bool draftCalled = false;
    bool reviewCalled = false;

    ForgeStateMachine sm(
        [&](ForgeJob&) { draftCalled = true; },
        [&](ForgeJob&) { reviewCalled = true; },
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [](ForgeJob&) {}
    );

    ForgeJob job;
    job.phase = "Drafting";
    sm.process(job);
    EXPECT_TRUE(draftCalled);
    EXPECT_EQ(job.phase, "Reviewing");
}

TEST(ForgeStateMachineTest, ReviewingToSandboxProbe) {
    bool reviewCalled = false;
    bool probeCalled = false;

    ForgeStateMachine sm(
        [](ForgeJob&) {},
        [&](ForgeJob&) { reviewCalled = true; },
        [&](ForgeJob&) { probeCalled = true; },
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [](ForgeJob&) {}
    );

    ForgeJob job;
    job.phase = "Reviewing";
    sm.process(job);
    EXPECT_TRUE(reviewCalled);
    EXPECT_EQ(job.phase, "SandboxProbe");
}

TEST(ForgeStateMachineTest, SandboxProbeToApproved) {
    bool probeCalled = false;
    bool approveCalled = false;

    ForgeStateMachine sm(
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [&](ForgeJob&) { probeCalled = true; },
        [&](ForgeJob&) { approveCalled = true; },
        [](ForgeJob&) {},
        [](ForgeJob&) {}
    );

    ForgeJob job;
    job.phase = "SandboxProbe";
    sm.process(job);
    EXPECT_TRUE(probeCalled);
    EXPECT_EQ(job.phase, "Approved");
}

TEST(ForgeStateMachineTest, ApprovedToPromoted) {
    bool approveCalled = false;
    bool promoteCalled = false;

    ForgeStateMachine sm(
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [&](ForgeJob&) { approveCalled = true; },
        [&](ForgeJob&) { promoteCalled = true; },
        [](ForgeJob&) {}
    );

    ForgeJob job;
    job.phase = "Approved";
    sm.process(job);
    EXPECT_TRUE(approveCalled);
    EXPECT_EQ(job.phase, "Promoted");
}

TEST(ForgeStateMachineTest, PromotedStaysPromoted) {
    bool promoteCalled = false;

    ForgeStateMachine sm(
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [&](ForgeJob&) { promoteCalled = true; },
        [](ForgeJob&) {}
    );

    ForgeJob job;
    job.phase = "Promoted";
    sm.process(job);
    EXPECT_TRUE(promoteCalled);
    EXPECT_EQ(job.phase, "Promoted");
}

TEST(ForgeStateMachineTest, HumanReviewCallsCallback) {
    bool humanReviewCalled = false;

    ForgeStateMachine sm(
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [](ForgeJob&) {},
        [&](ForgeJob&) { humanReviewCalled = true; }
    );

    ForgeJob job;
    job.phase = "HumanReview";
    sm.process(job);
    EXPECT_TRUE(humanReviewCalled);
    EXPECT_EQ(job.phase, "HumanReview");
}

// -----------------------------------------------------------------------
// Test ForgeManager (requires full mock of Registry, Dispatcher, etc.)
// -----------------------------------------------------------------------
class ForgeManagerTest : public ::testing::Test {
protected:
    MockDatabase mockDb;
    Registry registry{mockDb};
    Dispatcher dispatcher{"/tmp/forge_test"};
    Orchestrator orchestrator{registry, verifier, scheduler, dispatcher, ""};
    ObsBus obsBus;
    ForgeManager forgeManager{mockDb, registry, dispatcher, orchestrator, obsBus};

    void SetUp() override {
        forgeManager.initialize();
    }
};

TEST_F(ForgeManagerTest, CreateJob) {
    std::string id = forgeManager.create_job("test_method", "test_requirement");
    EXPECT_FALSE(id.empty());
    EXPECT_TRUE(id.find("forge_") == 0);
}

TEST_F(ForgeManagerTest, GetJob) {
    std::string id = forgeManager.create_job("method1", "req1");
    auto opt = forgeManager.get_job(id);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->method, "method1");
    EXPECT_EQ(opt->requirement, "req1");
    EXPECT_EQ(opt->phase, "Drafting");
    EXPECT_EQ(opt->attempt, 0);
}

TEST_F(ForgeManagerTest, ProcessJob) {
    std::string id = forgeManager.create_job("method2", "req2");
    forgeManager.process_job(id);
    auto opt = forgeManager.get_job(id);
    ASSERT_TRUE(opt.has_value());
    // After processing, phase should be Reviewing (since Drafting -> Reviewing)
    EXPECT_EQ(opt->phase, "Reviewing");
    EXPECT_EQ(opt->attempt, 1);
}

TEST_F(ForgeManagerTest, ListJobs) {
    forgeManager.create_job("m1", "r1");
    forgeManager.create_job("m2", "r2");
    auto jobs = forgeManager.list_jobs();
    EXPECT_EQ(jobs.size(), 2);
}

TEST_F(ForgeManagerTest, ListHumanReviewJobs) {
    // Create a job and manually set its phase to HumanReview
    std::string id = forgeManager.create_job("m3", "r3");
    auto opt = forgeManager.get_job(id);
    ASSERT_TRUE(opt.has_value());
    opt->phase = "HumanReview";
    // Use forgeDb directly to update
    ForgeDatabase forgeDb(mockDb);
    forgeDb.update_job(*opt);

    auto humanJobs = forgeManager.list_human_review_jobs();
    EXPECT_EQ(humanJobs.size(), 1);
    EXPECT_EQ(humanJobs[0].id, id);
}

TEST_F(ForgeManagerTest, ApproveHumanReview) {
    std::string id = forgeManager.create_job("m4", "r4");
    // Manually set phase to HumanReview
    ForgeDatabase forgeDb(mockDb);
    auto opt = forgeDb.get_job(id);
    ASSERT_TRUE(opt.has_value());
    opt->phase = "HumanReview";
    forgeDb.update_job(*opt);

    forgeManager.approve_human_review(id);
    auto updated = forgeManager.get_job(id);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->phase, "Approved");
}

TEST_F(ForgeManagerTest, RejectHumanReview) {
    std::string id = forgeManager.create_job("m5", "r5");
    ForgeDatabase forgeDb(mockDb);
    auto opt = forgeDb.get_job(id);
    ASSERT_TRUE(opt.has_value());
    opt->phase = "HumanReview";
    forgeDb.update_job(*opt);

    forgeManager.reject_human_review(id, "bad code");
    auto updated = forgeManager.get_job(id);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->phase, "Drafting");
    EXPECT_EQ(updated->last_feedback, "bad code");
}
