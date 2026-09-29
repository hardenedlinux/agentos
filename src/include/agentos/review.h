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
#pragma once
/**
 * review.h — reviewer step output contract (ADR-031 §14, ADR-016).
 *
 * A step whose component is declared a reviewer (agents.is_reviewer) must
 * report its judgment in a reserved "review" object of its result:
 *
 *   "review": { "verdict": "pass" | "reject",
 *               "findings": [ { "code", "message", "detail"? } ] }
 *
 * This module only validates and reshapes that object; what happens to the
 * job is decided by the Orchestrator.
 */

#include <string>

namespace agentos
{
  struct ReviewCheck
  {
    bool valid = false;       // a well-formed review object was found
    bool rejected = false;    // valid && verdict == "reject"
    std::string findings_json = "[]"; // the findings array, re-serialised
    std::string error;        // why the output is invalid (valid == false)
  };

  // Validate the "review" member of a step result (a JSON object text).
  // `findings` may be absent (treated as []) and may be empty; every
  // finding must be an object with string "code" and "message" and, if
  // present, an object "detail".
  ReviewCheck check_review (const std::string &result_json);

  // The job-level review object persisted in jobs.review_json and exposed
  // as job.status "review" (ADR-039 §B):
  //   { "step_id", "reviewer_id", "verdict": "reject", "findings": [...] }
  std::string make_job_review_json (const std::string &step_id,
                                    const std::string &reviewer_id,
                                    const std::string &findings_json);
} // namespace agentos
