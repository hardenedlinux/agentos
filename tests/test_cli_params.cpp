/**
 * tests/test_cli_params.cpp
 *
 * Unit tests for all build_*_params() functions.
 * Pure functions — no ZMQ, no daemon, no database.
 */

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <string>

#include "agentos/job_params.h"
#include "agentos/worker_params.h"
#include "agentos/adviser_params.h"
#include "agentos/review_params.h"
#include "agentos/forge_params.h"

using namespace agentos::cli;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool has_string(const rapidjson::Document& d,
                       const char* key, const char* expected)
{
    return d.HasMember(key)
        && d[key].IsString()
        && std::string(d[key].GetString()) == expected;
}

static bool has_int(const rapidjson::Document& d, const char* key, int expected)
{
    return d.HasMember(key) && d[key].IsInt() && d[key].GetInt() == expected;
}

static bool has_bool(const rapidjson::Document& d, const char* key, bool expected)
{
    return d.HasMember(key) && d[key].IsBool() && d[key].GetBool() == expected;
}

// ---------------------------------------------------------------------------
// job.submit
// ---------------------------------------------------------------------------

TEST(JobParamsTest, SubmitOneshot_RequiredFieldsOnly)
{
    auto doc = build_job_submit_params(
        "do something", "oneshot", "", 0, 0, 5, "", "");

    EXPECT_TRUE(has_string(doc, "goal", "do something"));
    EXPECT_TRUE(has_string(doc, "type", "oneshot"));
    EXPECT_FALSE(doc.HasMember("input"));
    EXPECT_FALSE(doc.HasMember("schedule"));
    EXPECT_FALSE(doc.HasMember("loop"));
}

TEST(JobParamsTest, SubmitScheduled_HasScheduleBlock)
{
    auto doc = build_job_submit_params(
        "run daily", "scheduled", "", 86400, 1000000, 5, "", "");

    EXPECT_TRUE(has_string(doc, "type", "scheduled"));
    ASSERT_TRUE(doc.HasMember("schedule") && doc["schedule"].IsObject());
    EXPECT_EQ(doc["schedule"]["interval_s"].GetInt64(), 86400);
    EXPECT_EQ(doc["schedule"]["starts_at"].GetInt64(), 1000000);
    EXPECT_FALSE(doc.HasMember("loop"));
}

TEST(JobParamsTest, SubmitScheduled_NoStartsAt_OmitsField)
{
    auto doc = build_job_submit_params(
        "run daily", "scheduled", "", 86400, 0, 5, "", "");

    ASSERT_TRUE(doc.HasMember("schedule"));
    EXPECT_FALSE(doc["schedule"].HasMember("starts_at"));
}

TEST(JobParamsTest, SubmitLoop_HasLoopBlock)
{
    auto doc = build_job_submit_params(
        "loop job", "loop", "", 0, 0, 3, "rev-123", "output must be valid JSON");

    EXPECT_TRUE(has_string(doc, "type", "loop"));
    ASSERT_TRUE(doc.HasMember("loop") && doc["loop"].IsObject());
    EXPECT_EQ(doc["loop"]["max_iterations"].GetInt(), 3);
    EXPECT_STREQ(doc["loop"]["reviewer_id"].GetString(), "rev-123");
    EXPECT_STREQ(doc["loop"]["acceptance_criteria"].GetString(), "output must be valid JSON");
    EXPECT_FALSE(doc.HasMember("schedule"));
}

TEST(JobParamsTest, SubmitLoop_EmptyReviewerOmitted)
{
    auto doc = build_job_submit_params(
        "loop job", "loop", "", 0, 0, 5, "", "");

    ASSERT_TRUE(doc.HasMember("loop"));
    EXPECT_FALSE(doc["loop"].HasMember("reviewer_id"));
    EXPECT_FALSE(doc["loop"].HasMember("acceptance_criteria"));
}

TEST(JobParamsTest, SubmitWithValidJsonInput)
{
    auto doc = build_job_submit_params(
        "goal", "oneshot", R"({"key":"value"})", 0, 0, 5, "", "");

    ASSERT_TRUE(doc.HasMember("input"));
    ASSERT_TRUE(doc["input"].IsObject());
    EXPECT_STREQ(doc["input"]["key"].GetString(), "value");
}

TEST(JobParamsTest, SubmitWithInvalidJsonInput_InputOmitted)
{
    auto doc = build_job_submit_params(
        "goal", "oneshot", "not-json{{{", 0, 0, 5, "", "");

    EXPECT_FALSE(doc.HasMember("input"));
}

// ---------------------------------------------------------------------------
// job.status
// ---------------------------------------------------------------------------

TEST(JobParamsTest, Status_HasJobId)
{
    auto doc = build_job_status_params("job-abc-123");
    EXPECT_TRUE(has_string(doc, "job_id", "job-abc-123"));
}

// ---------------------------------------------------------------------------
// job.list
// ---------------------------------------------------------------------------

TEST(JobParamsTest, List_DefaultsOnly)
{
    auto doc = build_job_list_params("", "", 50, 0);
    EXPECT_FALSE(doc.HasMember("phase"));
    EXPECT_FALSE(doc.HasMember("type"));
    EXPECT_TRUE(has_int(doc, "limit", 50));
    EXPECT_TRUE(has_int(doc, "offset", 0));
}

TEST(JobParamsTest, List_WithFilters)
{
    auto doc = build_job_list_params("executing", "loop", 10, 20);
    EXPECT_TRUE(has_string(doc, "phase", "executing"));
    EXPECT_TRUE(has_string(doc, "type",  "loop"));
    EXPECT_TRUE(has_int(doc, "limit",  10));
    EXPECT_TRUE(has_int(doc, "offset", 20));
}

// ---------------------------------------------------------------------------
// job.cancel
// ---------------------------------------------------------------------------

TEST(JobParamsTest, Cancel_StopScheduleTrue)
{
    auto doc = build_job_cancel_params("job-xyz", true);
    EXPECT_TRUE(has_string(doc, "job_id", "job-xyz"));
    EXPECT_TRUE(has_bool(doc, "stop_schedule", true));
}

TEST(JobParamsTest, Cancel_StopScheduleFalse)
{
    auto doc = build_job_cancel_params("job-xyz", false);
    EXPECT_TRUE(has_bool(doc, "stop_schedule", false));
}

// ---------------------------------------------------------------------------
// worker
// ---------------------------------------------------------------------------

TEST(WorkerParamsTest, Register_HasPath)
{
    auto doc = build_worker_register_params("/home/user/.agentos/workers/my-worker");
    EXPECT_TRUE(has_string(doc, "path", "/home/user/.agentos/workers/my-worker"));
}

TEST(WorkerParamsTest, List_NoFilter_EmptyObject)
{
    auto doc = build_worker_list_params("");
    EXPECT_FALSE(doc.HasMember("enabled"));
}

TEST(WorkerParamsTest, List_WithEnabledFilter)
{
    auto doc = build_worker_list_params("true");
    EXPECT_TRUE(has_string(doc, "enabled", "true"));
}

TEST(WorkerParamsTest, Toggle_HasWorkerId)
{
    auto doc = build_worker_toggle_params("worker-001");
    EXPECT_TRUE(has_string(doc, "worker_id", "worker-001"));
}

// ---------------------------------------------------------------------------
// adviser
// ---------------------------------------------------------------------------

TEST(AdviserParamsTest, Register_HasPath)
{
    auto doc = build_adviser_register_params("/home/user/.agentos/advisers/my-adviser");
    EXPECT_TRUE(has_string(doc, "path", "/home/user/.agentos/advisers/my-adviser"));
}

TEST(AdviserParamsTest, List_IsEmptyObject)
{
    auto doc = build_adviser_list_params();
    EXPECT_TRUE(doc.IsObject());
    EXPECT_TRUE(doc.ObjectEmpty());
}

// ---------------------------------------------------------------------------
// review
// ---------------------------------------------------------------------------

TEST(ReviewParamsTest, List_NoFilters)
{
    auto doc = build_review_list_params("", "");
    EXPECT_FALSE(doc.HasMember("status"));
    EXPECT_FALSE(doc.HasMember("type"));
}

TEST(ReviewParamsTest, List_WithFilters)
{
    auto doc = build_review_list_params("pending", "auto");
    EXPECT_TRUE(has_string(doc, "status", "pending"));
    EXPECT_TRUE(has_string(doc, "type",   "auto"));
}

TEST(ReviewParamsTest, List_OnlyStatus)
{
    auto doc = build_review_list_params("approved", "");
    EXPECT_TRUE(has_string(doc, "status", "approved"));
    EXPECT_FALSE(doc.HasMember("type"));
}

TEST(ReviewParamsTest, IdParams_HasReviewId)
{
    auto doc = build_review_id_params("rev-999");
    EXPECT_TRUE(has_string(doc, "review_id", "rev-999"));
}

TEST(ReviewParamsTest, Reject_HasReviewIdAndMessage)
{
    auto doc = build_review_reject_params("rev-001", "code is wrong");
    EXPECT_TRUE(has_string(doc, "review_id", "rev-001"));
    EXPECT_TRUE(has_string(doc, "message",   "code is wrong"));
}

TEST(ReviewParamsTest, Reject_EmptyMessageStillIncluded)
{
    // message is required by CLI so will never truly be empty at runtime,
    // but the build function must not silently drop it
    auto doc = build_review_reject_params("rev-002", "");
    EXPECT_TRUE(doc.HasMember("message"));
}

// ---------------------------------------------------------------------------
// forge
// ---------------------------------------------------------------------------

TEST(ForgeParamsTest, List_NoPhase_EmptyObject)
{
    auto doc = build_forge_list_params("");
    EXPECT_FALSE(doc.HasMember("phase"));
}

TEST(ForgeParamsTest, List_WithPhase)
{
    auto doc = build_forge_list_params("reviewing");
    EXPECT_TRUE(has_string(doc, "phase", "reviewing"));
}

TEST(ForgeParamsTest, Status_HasForgeId)
{
    auto doc = build_forge_status_params("forge-abc");
    EXPECT_TRUE(has_string(doc, "forge_id", "forge-abc"));
}
