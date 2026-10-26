/**
 * Copyright (C) 2026  HardenedLinux community
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// ADR-031 §11 amendment: Adviser output repair.

#include "agentos/step_repair.h"

#include <gtest/gtest.h>

using namespace agentos;

namespace
{
  Step make_step (const std::string &id, const std::string &target_type,
                  const std::string &result = "{}",
                  const std::string &command = "cmd")
  {
    Step s;
    s.id = id;
    s.target_type = target_type;
    s.result_json = result;
    s.command = command;
    return s;
  }

  // a1 (adviser) -> w2 (worker) -> a3 (adviser) -> w4 (worker, failing)
  std::vector<Step> plan ()
  {
    return {make_step ("a1", "adviser", R"({"css":"a{"})", "style-adviser"),
            make_step ("w2", "worker", R"({"ok":true})"),
            make_step ("a3", "adviser", R"({"html":"<p>"})", "copy-adviser"),
            make_step ("w4", "worker", "")};
  }
} // namespace

TEST (RepairCandidatesTest, NoReferencesMeansPreviousStep)
{
  auto ids = repair_candidates (plan (), "w4", {{"mode", "fast"}});
  ASSERT_EQ (ids.size (), 1u);
  EXPECT_EQ (ids[0], "a3");
}

TEST (RepairCandidatesTest, ExplicitStepReferenceOnly)
{
  auto ids = repair_candidates (plan (), "w4", {{"css", "$step:a1.css"}});
  ASSERT_EQ (ids.size (), 1u);
  EXPECT_EQ (ids[0], "a1");
}

TEST (RepairCandidatesTest, SeveralReferencesInPlanOrderDeduplicated)
{
  auto ids = repair_candidates (plan (), "w4",
                                {{"html", "$prev_result"},
                                 {"css", "$step:a1.css"},
                                 {"css_all", "$step:a1"},
                                 {"other", "$step:w2.ok"}});
  ASSERT_EQ (ids.size (), 2u);
  EXPECT_EQ (ids[0], "a1");
  EXPECT_EQ (ids[1], "a3");
}

TEST (RepairCandidatesTest, WorkerPredecessorIsNotACandidate)
{
  std::vector<Step> p = {make_step ("a1", "adviser"),
                         make_step ("w2", "worker"),
                         make_step ("w3", "worker")};
  EXPECT_TRUE (repair_candidates (p, "w3", {}).empty ());
}

TEST (RepairCandidatesTest, LaterOrUnknownStepsAreIgnored)
{
  // A reference to a later step (or to the failing step itself) is never
  // a source of the failing step's input.
  EXPECT_TRUE (
    repair_candidates (plan (), "w2", {{"x", "$step:a3.html"}}).empty ());
  EXPECT_TRUE (repair_candidates (plan (), "nope", {}).empty ());
  EXPECT_TRUE (repair_candidates (plan (), "a1", {}).empty ());
}

TEST (RepairCandidatesTest, AdviserWithoutResultIsSkipped)
{
  std::vector<Step> p = {make_step ("a1", "adviser", ""),
                         make_step ("w2", "worker")};
  EXPECT_TRUE (repair_candidates (p, "w2", {}).empty ());
}

TEST (PreviousStepIdTest, Basics)
{
  EXPECT_EQ (previous_step_id (plan (), "w4"), "a3");
  EXPECT_EQ (previous_step_id (plan (), "a1"), "");
  EXPECT_EQ (previous_step_id (plan (), "nope"), "");
}

TEST (RepairPromptTest, CarriesErrorAndSources)
{
  std::vector<RepairSource> src = {{"a1", "style-adviser", R"({"css":"a{"})"}};
  auto p = repair_user_prompt ("w4", "facade.compose_css", "compose",
                               "CSS has unbalanced braces", src);
  EXPECT_NE (p.find ("facade.compose_css"), std::string::npos);
  EXPECT_NE (p.find ("CSS has unbalanced braces"), std::string::npos);
  EXPECT_NE (p.find ("Step a1 (Adviser style-adviser)"), std::string::npos);
  EXPECT_NE (p.find (R"({"css":"a{"})"), std::string::npos);
}

TEST (RepairPromptTest, LongErrorIsTruncated)
{
  std::string err (10000, 'x');
  auto p = repair_user_prompt ("w", "c", "", err, {});
  EXPECT_LT (p.size (), 5000u);
  EXPECT_NE (p.find ("[truncated]"), std::string::npos);
}

TEST (ParseRepairReplyTest, AcceptsChangedObject)
{
  std::vector<RepairSource> src = {{"a1", "x", R"({"css":"a{","n":1})"}};
  auto r = parse_repair_reply (R"({"repairs":{"a1":{"css":"a{}","n":1}}})",
                               src);
  ASSERT_TRUE (r.ok) << r.error;
  ASSERT_EQ (r.repairs.size (), 1u);
  EXPECT_EQ (r.repairs[0].first, "a1");
  EXPECT_EQ (r.repairs[0].second, R"({"css":"a{}","n":1})");
}

TEST (ParseRepairReplyTest, ToleratesCodeFence)
{
  std::vector<RepairSource> src = {{"a1", "x", R"({"css":"a{"})"}};
  auto r = parse_repair_reply (
    "```json\n{\"repairs\":{\"a1\":{\"css\":\"a{}\"}}}\n```\n", src);
  ASSERT_TRUE (r.ok) << r.error;
  EXPECT_EQ (r.repairs[0].second, R"({"css":"a{}"})");
}

TEST (ParseRepairReplyTest, UnchangedIsRejected)
{
  // Whitespace differences do not count as a change.
  std::vector<RepairSource> src = {{"a1", "x", R"({ "css" : "a{" })"}};
  auto r = parse_repair_reply (R"({"repairs":{"a1":{"css":"a{"}}})", src);
  EXPECT_FALSE (r.ok);
  EXPECT_TRUE (r.repairs.empty ());
}

TEST (ParseRepairReplyTest, UnchangedEntriesAreDropped)
{
  std::vector<RepairSource> src = {{"a1", "x", R"({"v":1})"},
                                   {"a3", "y", R"({"v":2})"}};
  auto r = parse_repair_reply (R"({"repairs":{"a1":{"v":1},"a3":{"v":3}}})",
                               src);
  ASSERT_TRUE (r.ok) << r.error;
  ASSERT_EQ (r.repairs.size (), 1u);
  EXPECT_EQ (r.repairs[0].first, "a3");
}

TEST (ParseRepairReplyTest, UnknownStepRejectsWholeReply)
{
  std::vector<RepairSource> src = {{"a1", "x", R"({"v":1})"}};
  auto r = parse_repair_reply (R"({"repairs":{"a1":{"v":2},"w2":{"v":3}}})",
                               src);
  EXPECT_FALSE (r.ok);
  EXPECT_TRUE (r.repairs.empty ());
}

TEST (ParseRepairReplyTest, MalformedReplies)
{
  std::vector<RepairSource> src = {{"a1", "x", R"({"v":1})"}};
  EXPECT_FALSE (parse_repair_reply ("not json", src).ok);
  EXPECT_FALSE (parse_repair_reply ("[]", src).ok);
  EXPECT_FALSE (parse_repair_reply (R"({"v":2})", src).ok);
  EXPECT_FALSE (parse_repair_reply (R"({"repairs":{}})", src).ok);
  EXPECT_FALSE (parse_repair_reply (R"({"repairs":{"a1":"text"}})", src).ok);
  EXPECT_FALSE (parse_repair_reply (R"({"repairs":{"a1":[1]}})", src).ok);
}
