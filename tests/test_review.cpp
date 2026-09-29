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

// ADR-031 §14.3: reviewer output contract.

#include "agentos/review.h"

#include <gtest/gtest.h>

using agentos::check_review;
using agentos::make_job_review_json;

TEST (ReviewCheckTest, PassWithoutFindings)
{
  auto rc = check_review (R"({"review":{"verdict":"pass"},"other":1})");
  EXPECT_TRUE (rc.valid);
  EXPECT_FALSE (rc.rejected);
  EXPECT_EQ (rc.findings_json, "[]");
}

TEST (ReviewCheckTest, RejectWithFindings)
{
  auto rc = check_review (
    R"({"review":{"verdict":"reject","findings":[)"
    R"({"code":"contrast.aa_fail","message":"too low","detail":{"ratio":2.1}},)"
    R"({"code":"c2","message":"m2"}]}})");
  EXPECT_TRUE (rc.valid) << rc.error;
  EXPECT_TRUE (rc.rejected);
  EXPECT_EQ (rc.findings_json,
             R"([{"code":"contrast.aa_fail","message":"too low","detail":{"ratio":2.1}},)"
             R"({"code":"c2","message":"m2"}])");
}

TEST (ReviewCheckTest, MalformedIsInvalid)
{
  for (const char *bad : {
         "not json",
         "[]",
         R"({"verdict":"pass"})",                        // not under review
         R"({"review":"pass"})",
         R"({"review":{}})",
         R"({"review":{"verdict":"maybe"}})",
         R"({"review":{"verdict":"reject","findings":{}}})",
         R"({"review":{"verdict":"reject","findings":[{"code":"c"}]}})",
         R"({"review":{"verdict":"reject","findings":["x"]}})",
         R"({"review":{"verdict":"reject","findings":[{"code":"c","message":"m","detail":1}]}})",
       })
  {
    auto rc = check_review (bad);
    EXPECT_FALSE (rc.valid) << bad;
    EXPECT_FALSE (rc.rejected) << bad;
    EXPECT_FALSE (rc.error.empty ()) << bad;
  }
}

TEST (ReviewCheckTest, JobReviewObject)
{
  EXPECT_EQ (make_job_review_json ("step-2", "contrast-check",
                                   R"([{"code":"c","message":"m"}])"),
             R"({"step_id":"step-2","reviewer_id":"contrast-check",)"
             R"("verdict":"reject","findings":[{"code":"c","message":"m"}]})");
  EXPECT_EQ (make_job_review_json ("s", "r", "garbage"),
             R"({"step_id":"s","reviewer_id":"r","verdict":"reject","findings":[]})");
}
