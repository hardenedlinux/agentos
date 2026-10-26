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
 * step_repair.h — Adviser output repair (ADR-031 §11 amendment).
 *
 * When a Worker-target step reports status "error" (ADR-016) after
 * consuming an Adviser-target step's output, the failure is usually a
 * small defect in that output (unbalanced braces, a missing field). Instead
 * of failing the job, the Orchestrator asks the LLM to correct the
 * consumed output, given the Worker's error, then runs the Worker again.
 *
 * This module holds the pure parts: which steps' outputs to repair, the
 * prompt, and validation of the LLM's reply. The Orchestrator owns the
 * LLM call, persistence and re-dispatch.
 */

#include "agentos/protocol_types.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agentos
{
  // One Adviser output the failing step consumed.
  struct RepairSource
  {
    std::string step_id;
    std::string adviser_id;
    std::string result_json; // the persisted tasks.result (a JSON object)
  };

  // Ids of the Adviser-target steps whose results the failing step
  // consumed, in plan order, without duplicates. `steps` is the job's
  // plan (Database::load_steps_for_job); `params` is the failing step's
  // unresolved params. If the params reference results explicitly
  // ($prev_result, $step:<id>, $step:<id>.<field>), exactly those steps
  // are candidates. Otherwise the immediately preceding step is, since a
  // Worker's task_json always carries it as "$prev_result". Steps that are
  // not Adviser-target, have no result, or come at or after the failing
  // step are never candidates.
  std::vector<std::string>
  repair_candidates (const std::vector<Step> &steps,
                     const std::string &failing_step_id,
                     const std::unordered_map<std::string, std::string> &params);

  // Id of the step right before `step_id` in plan order; empty if none.
  std::string previous_step_id (const std::vector<Step> &steps,
                                const std::string &step_id);

  std::string repair_system_prompt ();

  // `error` is the Worker's reported error, truncated to a bounded size.
  std::string repair_user_prompt (const std::string &step_id,
                                  const std::string &command,
                                  const std::string &description,
                                  const std::string &error,
                                  const std::vector<RepairSource> &sources);

  struct RepairParse
  {
    bool ok = false;
    // (step_id, corrected result JSON) for each output that changed.
    std::vector<std::pair<std::string, std::string>> repairs;
    std::string error; // why the reply is unusable (ok == false)
  };

  // Validate the LLM reply {"repairs": {"<step_id>": {...}, ...}}. Every
  // key must name one of `sources` and every value must be a JSON object.
  // Entries identical to the original are dropped; a reply that changes
  // nothing is not ok, since running the Worker again would fail the same
  // way. A surrounding Markdown code fence is tolerated.
  RepairParse parse_repair_reply (const std::string &content,
                                  const std::vector<RepairSource> &sources);
} // namespace agentos
