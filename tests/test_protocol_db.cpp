#include "agentos/database.h"
#include "agentos/protocol_types.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace agentos;

class ProtocolDbTest : public ::testing::Test
{
protected:
  void SetUp () override
  {
    // use an in‑memory SQLite database
    ASSERT_TRUE (db_.open ());
  }

  Database db_{":memory:"};
};

// ---------------------------------------------------------------------------
// --- Job ----------------------------------------------------------------
// ---------------------------------------------------------------------------

TEST_F (ProtocolDbTest, InsertAndLoadJob_Oneshot)
{
  Job job;
  job.id         = "job-1";
  job.type       = std::string (db::job_type::oneshot);
  job.goal       = "transcribe audio";
  job.tags       = { "audio", "urgent" };
  job.phase      = std::string (db::job_phase::planning);
  job.created_at = 1700000000;
  job.updated_at = 1700000000;

  db_.insert_job (job);

  auto loaded = db_.load_job ("job-1");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->id,         job.id);
  EXPECT_EQ (loaded->type,       job.type);
  EXPECT_EQ (loaded->goal,       job.goal);
  EXPECT_EQ (loaded->tags,       job.tags);
  EXPECT_EQ (loaded->phase,      job.phase);
  EXPECT_EQ (loaded->created_at, job.created_at);
  EXPECT_EQ (loaded->updated_at, job.updated_at);
  EXPECT_FALSE (loaded->error.has_value ());
  EXPECT_FALSE (loaded->schedule.has_value ());
  EXPECT_FALSE (loaded->loop.has_value ());
}

TEST_F (ProtocolDbTest, InsertAndLoadJob_Loop)
{
  Job job;
  job.id    = "loop-job";
  job.type  = std::string (db::job_type::loop);
  job.goal  = "generate report";
  job.phase = std::string (db::job_phase::planning);
  job.tags  = { "report" };

  Loop loop;
  loop.max_iterations      = 5;
  loop.current_iteration   = 0;
  loop.max_repairs         = 2;
  loop.current_repairs     = 0;
  loop.reviewer_id         = "advisor-r1";
  loop.acceptance_criteria = "no errors";
  loop.last_feedback       = std::nullopt;
  job.loop = loop;

  job.schedule = std::nullopt;

  db_.insert_job (job);

  auto loaded = db_.load_job ("loop-job");
  ASSERT_TRUE (loaded.has_value ());
  ASSERT_TRUE (loaded->loop.has_value ());
  EXPECT_EQ (loaded->loop->max_iterations,      5);
  EXPECT_EQ (loaded->loop->current_iteration,   0);
  EXPECT_EQ (loaded->loop->max_repairs,         2);
  EXPECT_EQ (loaded->loop->current_repairs,     0);
  EXPECT_EQ (loaded->loop->reviewer_id,         "advisor-r1");
  EXPECT_EQ (loaded->loop->acceptance_criteria, "no errors");
  EXPECT_FALSE (loaded->loop->last_feedback.has_value ());
}

TEST_F (ProtocolDbTest, UpdateJobPhase)
{
  Job job;
  job.id    = "phase-job";
  job.type  = std::string (db::job_type::oneshot);
  job.goal  = "eval test";
  job.phase = std::string (db::job_phase::planning);
  job.created_at = 1700000000;
  job.updated_at = 1700000000;

  db_.insert_job (job);

  db_.update_job_phase ("phase-job", db::job_phase::planning,
                        db::job_phase::executing);

  auto loaded = db_.load_job ("phase-job");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->phase, std::string(db::job_phase::executing));
  EXPECT_GT (loaded->updated_at, job.updated_at);
}

TEST_F (ProtocolDbTest, IncrementIteration)
{
  Job job;
  job.id    = "iter-job";
  job.type  = std::string (db::job_type::loop);
  job.goal  = "iter test";
  job.phase = std::string (db::job_phase::planning);

  Loop l;
  l.max_iterations    = 5;
  l.current_iteration = 0;
  l.reviewer_id       = "r";
  l.acceptance_criteria = "ok";
  job.loop = l;

  db_.insert_job (job);

  db_.increment_job_iteration ("iter-job");
  db_.increment_job_iteration ("iter-job");

  auto loaded = db_.load_job ("iter-job");
  ASSERT_TRUE (loaded.has_value ());
  ASSERT_TRUE (loaded->loop.has_value ());
  EXPECT_EQ (loaded->loop->current_iteration, 2);
}

// ---------------------------------------------------------------------------
// --- Step ---------------------------------------------------------------
// ---------------------------------------------------------------------------

TEST_F (ProtocolDbTest, InsertAndLoadStep)
{
  Step step;
  step.id          = "step-1";
  step.job_id      = "job-step";
  step.step_order  = 0;
  step.description = "fetch data";
  step.status      = std::string (db::step_status::pending);

  db_.insert_step (step);

  auto loaded = db_.load_step ("step-1");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->id,          step.id);
  EXPECT_EQ (loaded->job_id,      step.job_id);
  EXPECT_EQ (loaded->step_order,  step.step_order);
  EXPECT_EQ (loaded->description, step.description);
  EXPECT_EQ (loaded->status,      step.status);
  EXPECT_FALSE (loaded->started_at.has_value ());
  EXPECT_FALSE (loaded->completed_at.has_value ());
}

TEST_F (ProtocolDbTest, CompleteStep)
{
  Step step;
  step.id         = "step-comp";
  step.job_id     = "j";
  step.step_order = 0;
  step.description = "run";
  step.status      = std::string (db::step_status::pending);
  db_.insert_step (step);

  db_.complete_step ("step-comp", R"({"ok":true})");

  auto loaded = db_.load_step ("step-comp");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->status, std::string (db::step_status::done));
  auto res = db_.load_step_result_opt ("step-comp");
  ASSERT_TRUE (res.has_value ());
  EXPECT_EQ (*res, R"({"ok":true})");
}

TEST_F (ProtocolDbTest, LoadStepsForJob)
{
  const std::string job_id = "steps-job";

  for (int i = 0; i < 3; ++i)
  {
    Step s;
    s.id          = "step-" + std::to_string (i);
    s.job_id      = job_id;
    s.step_order  = i;
    s.description = "step #" + std::to_string (i);
    s.status      = std::string (db::step_status::pending);
    db_.insert_step (s);
  }

  auto steps = db_.load_steps_for_job (job_id);
  ASSERT_EQ (steps.size (), 3u);
  for (int i = 0; i < 3; ++i)
  {
    EXPECT_EQ (steps[i].step_order, i);
  }
}

TEST_F (ProtocolDbTest, LoadStepResult_Null)
{
  Step step;
  step.id          = "no-result";
  step.job_id      = "j";
  step.step_order  = 0;
  step.description = "no result";
  step.status      = std::string (db::step_status::pending);
  db_.insert_step (step);

  auto res = db_.load_step_result_opt ("no-result");
  EXPECT_FALSE (res.has_value ());
}

// ---------------------------------------------------------------------------
// --- HumanReview --------------------------------------------------------
// ---------------------------------------------------------------------------

TEST_F (ProtocolDbTest, InsertAndLoadHumanReview)
{
  HumanReview r;
  r.id        = "review-1";
  r.type      = std::string (db::review_type::auto_);
  r.forge_id  = "forge-a";
  r.job_id    = "job-b";
  r.reason    = "max repairs exceeded";
  r.artifacts = R"({"attempts":3})";
  r.status    = std::string (db::review_status::pending);
  r.decision  = std::nullopt;
  r.created_at = 1800000000;

  db_.insert_human_review (r);

  auto loaded = db_.load_human_review ("review-1");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->type,       r.type);
  EXPECT_TRUE (loaded->forge_id.has_value ());
  EXPECT_EQ (*loaded->forge_id,  "forge-a");
  EXPECT_TRUE (loaded->job_id.has_value ());
  EXPECT_EQ (*loaded->job_id,    "job-b");
  EXPECT_EQ (loaded->reason,     r.reason);
  EXPECT_EQ (loaded->artifacts,  r.artifacts);
  EXPECT_EQ (loaded->status,     r.status);
  EXPECT_FALSE (loaded->decision.has_value ());
  EXPECT_FALSE (loaded->reviewed_at.has_value ());
}

TEST_F (ProtocolDbTest, ApproveReview)
{
  HumanReview r;
  r.id        = "rev-approve";
  r.type      = std::string (db::review_type::human);
  r.reason    = "manual check";
  r.artifacts = "{}";
  r.status    = std::string (db::review_status::pending);
  db_.insert_human_review (r);

  db_.update_review_status ("rev-approve", db::review_status::approved,
                            "looks good");

  auto loaded = db_.load_human_review ("rev-approve");
  ASSERT_TRUE (loaded.has_value ());
  EXPECT_EQ (loaded->status, std::string (db::review_status::approved));
  ASSERT_TRUE (loaded->decision.has_value ());
  EXPECT_EQ (*loaded->decision, "looks good");
  EXPECT_TRUE (loaded->reviewed_at.has_value ());
}

// ---------------------------------------------------------------------------
// --- Active jobs & filtered list ----------------------------------------
// ---------------------------------------------------------------------------

TEST_F (ProtocolDbTest, LoadActiveJobs)
{
  auto makeJob = [] (const std::string &id, std::string_view phase) {
    Job j;
    j.id    = id;
    j.type  = std::string (db::job_type::oneshot);
    j.goal  = "test";
    j.phase = std::string (phase);
    return j;
  };

  db_.insert_job (makeJob ("active-1", db::job_phase::planning));
  db_.insert_job (makeJob ("active-2", db::job_phase::executing));
  db_.insert_job (makeJob ("done-1",   db::job_phase::done));

  auto active = db_.load_active_jobs ();
  ASSERT_EQ (active.size (), 2u);

  std::vector<std::string> ids;
  for (auto &j : active)
    ids.push_back (j.id);
  EXPECT_TRUE (std::find (ids.begin (), ids.end (), "active-1") != ids.end ());
  EXPECT_TRUE (std::find (ids.begin (), ids.end (), "active-2") != ids.end ());
}

TEST_F (ProtocolDbTest, LoadJobs_FilterByPhase)
{
  auto makeJob = [] (const std::string &id, std::string_view phase) {
    Job j;
    j.id    = id;
    j.type  = std::string (db::job_type::oneshot);
    j.goal  = "filter";
    j.phase = std::string (phase);
    return j;
  };

  db_.insert_job (makeJob ("p1", db::job_phase::planning));
  db_.insert_job (makeJob ("e1", db::job_phase::executing));

  auto filtered = db_.load_jobs (std::nullopt,
                                 db::job_phase::executing,
                                 10, 0);
  ASSERT_EQ (filtered.size (), 1u);
  EXPECT_EQ (filtered[0].id, "e1");
}
