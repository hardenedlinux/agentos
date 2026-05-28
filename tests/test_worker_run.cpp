#include <gtest/gtest.h>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>

#include "agentos/database/database.h"
#include "agentos/sandbox.h"
#include "agentos/forge/forge_database.h"
#include "agentos/orchestrator.h"
#include "agentos/home_init.h"
#include "agentos/registry.h"
#include "agentos/verifier.h"
#include "agentos/scheduler.h"
#include "agentos/dispatcher.h"
#include "agentos/types.h"

using namespace agentos;

// ---------------------------------------------------------------------------
// Helper: create a temporary directory and set AGENTOS_HOME to it
// ---------------------------------------------------------------------------
class TempHome {
public:
    TempHome() {
        char tmpl[] = "/tmp/agentos_test_XXXXXX";
        char* dir = mkdtemp(tmpl);
        if (!dir) throw std::runtime_error("mkdtemp failed");
        path_ = dir;
        old_home_ = std::getenv("AGENTOS_HOME") ? std::getenv("AGENTOS_HOME") : "";
        setenv("AGENTOS_HOME", path_.c_str(), 1);
    }
    ~TempHome() {
        if (!old_home_.empty())
            setenv("AGENTOS_HOME", old_home_.c_str(), 1);
        else
            unsetenv("AGENTOS_HOME");
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::string& path() const { return path_; }
private:
    std::string path_;
    std::string old_home_;
};

// ---------------------------------------------------------------------------
// Dummy stubs for Orchestrator dependencies
// ---------------------------------------------------------------------------
class DummyRegistry : public Registry {
public:
    DummyRegistry() : Registry() {}
};

class DummyVerifier : public Verifier {
public:
    DummyVerifier(const Registry& r) : Verifier(r) {}
};

class DummyScheduler : public Scheduler {
public:
    DummyScheduler(const Registry& r, Dispatcher& d, const SchedulerConfig& c = {})
        : Scheduler(r, d, c) {}
};

class DummyDispatcher : public Dispatcher {
public:
    DummyDispatcher() : Dispatcher("/tmp") {}
};

// ===========================================================================
// Tests for apply_worker_filesystem
// ===========================================================================
TEST(WorkerRunTest, ApplyWorkerFilesystemCreatesDirectories) {
    // This test requires root privileges because it calls mount/pivot_root.
    // Skip if not root.
    if (geteuid() != 0) {
        GTEST_SKIP() << "Skipping test that requires root privileges";
    }

    TempHome home;
    std::string worker_id = "test_worker";
    std::string run_id = "test_run_001";

    // Call the function
    bool result = apply_worker_filesystem(worker_id, run_id);
    EXPECT_TRUE(result);

    // Verify directories were created
    std::filesystem::path base(home.path());
    std::filesystem::path layers_dir = base / "layers" / "runs" / run_id;
    std::filesystem::path upper_dir = layers_dir / "upper";
    std::filesystem::path work_dir  = layers_dir / "work";
    std::filesystem::path log_dir  = base / "logs" / "runs" / run_id;

    EXPECT_TRUE(std::filesystem::exists(upper_dir));
    EXPECT_TRUE(std::filesystem::exists(work_dir));
    EXPECT_TRUE(std::filesystem::exists(log_dir));
}

// ===========================================================================
// Tests for gc_run_layers
// ===========================================================================
TEST(WorkerRunTest, GcRunLayersRemovesNonRunningLayers) {
    TempHome home;

    // Create a temporary layer directory
    std::string layer_path = home.path() + "/layers/runs/test_run_gc";
    std::filesystem::create_directories(layer_path);
    ASSERT_TRUE(std::filesystem::exists(layer_path));

    // Create a WorkerRun with status "completed"
    WorkerRun run;
    run.run_id = "test_run_gc";
    run.worker_id = "test_worker";
    run.status = "completed";
    run.layer_path = layer_path;
    run.log_path = home.path() + "/logs/runs/test_run_gc/output.log";

    // Use a real Database with in-memory SQLite
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    // Insert the run
    db.insert_worker_run(run);

    // Call gc_run_layers
    gc_run_layers(db);

    // The layer directory should be removed
    EXPECT_FALSE(std::filesystem::exists(layer_path));
}

TEST(WorkerRunTest, GcRunLayersDoesNotRemoveRunningLayers) {
    TempHome home;

    std::string layer_path = home.path() + "/layers/runs/test_run_running";
    std::filesystem::create_directories(layer_path);
    ASSERT_TRUE(std::filesystem::exists(layer_path));

    WorkerRun run;
    run.run_id = "test_run_running";
    run.worker_id = "test_worker";
    run.status = "running";
    run.layer_path = layer_path;
    run.log_path = home.path() + "/logs/runs/test_run_running/output.log";

    Database db(":memory:");
    ASSERT_TRUE(db.open());
    db.insert_worker_run(run);

    gc_run_layers(db);

    // The layer directory should still exist
    EXPECT_TRUE(std::filesystem::exists(layer_path));

    // Cleanup
    std::filesystem::remove_all(layer_path);
}

// ===========================================================================
// Tests for Database worker_runs methods
// ===========================================================================
TEST(WorkerRunTest, DatabaseInsertAndGetActiveWorkerRuns) {
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    WorkerRun run;
    run.run_id = "run_001";
    run.worker_id = "worker_1";
    run.pid = 1234;
    run.started_at = 1000;
    run.status = "running";
    run.layer_path = "/tmp/layers/runs/run_001";
    run.log_path = "/tmp/logs/runs/run_001/output.log";

    db.insert_worker_run(run);

    auto active = db.get_active_worker_runs();
    ASSERT_EQ(active.size(), 1);
    EXPECT_EQ(active[0].run_id, "run_001");
    EXPECT_EQ(active[0].status, "running");
}

TEST(WorkerRunTest, DatabaseUpdateWorkerRun) {
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    WorkerRun run;
    run.run_id = "run_002";
    run.worker_id = "worker_2";
    run.pid = 5678;
    run.started_at = 2000;
    run.status = "running";
    run.layer_path = "/tmp/layers/runs/run_002";
    run.log_path = "/tmp/logs/runs/run_002/output.log";

    db.insert_worker_run(run);

    // Update status to completed
    run.status = "completed";
    run.ended_at = 3000;
    run.exit_code = 0;
    db.update_worker_run(run);

    auto active = db.get_active_worker_runs();
    EXPECT_TRUE(active.empty()); // because status is no longer 'running'
}

TEST(WorkerRunTest, DatabaseMarkAllRunningAsCrashed) {
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    WorkerRun run1;
    run1.run_id = "run_a";
    run1.worker_id = "w1";
    run1.pid = 111;
    run1.started_at = 100;
    run1.status = "running";
    run1.layer_path = "/tmp/layers/runs/run_a";
    run1.log_path = "/tmp/logs/runs/run_a/output.log";

    WorkerRun run2;
    run2.run_id = "run_b";
    run2.worker_id = "w2";
    run2.pid = 222;
    run2.started_at = 200;
    run2.status = "running";
    run2.layer_path = "/tmp/layers/runs/run_b";
    run2.log_path = "/tmp/logs/runs/run_b/output.log";

    db.insert_worker_run(run1);
    db.insert_worker_run(run2);

    db.mark_all_running_as_crashed();

    auto active = db.get_active_worker_runs();
    EXPECT_TRUE(active.empty());
}

// ===========================================================================
// Tests for ForgeDatabase last_code_path
// ===========================================================================
TEST(WorkerRunTest, ForgeDatabaseInsertAndGetWithLastCodePath) {
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    ForgeDatabase fdb(db);
    fdb.create_tables();

    ForgeJob job;
    job.id = ForgeJobId("forge_test_001");
    job.method = "test_method";
    job.requirement = "test requirement";
    job.phase = "Drafting";
    job.last_code = "print('hello')";
    job.last_code_path = "/tmp/forge/forge_test_001/attempt_1.py";
    job.created_at = 1000;
    job.updated_at = 1000;

    fdb.insert_job(job);

    auto opt = fdb.get_job("forge_test_001");
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->last_code_path, "/tmp/forge/forge_test_001/attempt_1.py");
}

TEST(WorkerRunTest, ForgeDatabaseUpdateLastCodePath) {
    Database db(":memory:");
    ASSERT_TRUE(db.open());

    ForgeDatabase fdb(db);
    fdb.create_tables();

    ForgeJob job;
    job.id = ForgeJobId("forge_test_002");
    job.method = "test_method";
    job.requirement = "test requirement";
    job.phase = "Drafting";
    job.last_code = "print('hello')";
    job.last_code_path = "/tmp/forge/forge_test_002/attempt_1.py";
    job.created_at = 2000;
    job.updated_at = 2000;

    fdb.insert_job(job);

    // Update last_code_path
    job.last_code_path = "/tmp/forge/forge_test_002/attempt_2.py";
    job.updated_at = 3000;
    fdb.update_job(job);

    auto opt = fdb.get_job("forge_test_002");
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->last_code_path, "/tmp/forge/forge_test_002/attempt_2.py");
}

// ===========================================================================
// Main
// ===========================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
