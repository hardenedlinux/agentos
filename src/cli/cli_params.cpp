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

/**
 * src/cli/cli_params.cpp
 *
 * Implementations of all build_*_params() functions used by command groups.
 * Pure functions: no I/O, no ZMQ, no side effects. Fully unit-testable.
 */

#include "agentos/adviser_params.h"
#include "agentos/forge_params.h"
#include "agentos/job_params.h"
#include "agentos/review_params.h"
#include "agentos/worker_params.h"

#include <rapidjson/document.h>

namespace agentos::cli
{

  // ---------------------------------------------------------------------------
  // Job
  // ---------------------------------------------------------------------------

  rapidjson::Document
  build_job_submit_params (const std::string &goal, const std::string &type,
                           const std::string &input_str, int64_t interval_s,
                           int64_t starts_at, int max_iterations,
                           const std::string &reviewer_id,
                           const std::string &acceptance_criteria)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();

    doc.AddMember ("goal", rapidjson::Value (goal.c_str (), a).Move (), a);
    doc.AddMember ("type", rapidjson::Value (type.c_str (), a).Move (), a);

    if (!input_str.empty ())
    {
      rapidjson::Document input;
      if (!input.Parse (input_str.c_str ()).HasParseError ())
      {
        rapidjson::Value v;
        v.CopyFrom (input, a);
        doc.AddMember ("input", v, a);
      }
    }

    if (type == "scheduled")
    {
      rapidjson::Value sched (rapidjson::kObjectType);
      sched.AddMember ("interval_s", interval_s, a);
      if (starts_at > 0)
        sched.AddMember ("starts_at", starts_at, a);
      doc.AddMember ("schedule", sched, a);
    }

    if (type == "loop")
    {
      rapidjson::Value loop (rapidjson::kObjectType);
      loop.AddMember ("max_iterations", max_iterations, a);
      if (!reviewer_id.empty ())
        loop.AddMember ("reviewer_id",
                        rapidjson::Value (reviewer_id.c_str (), a).Move (), a);
      if (!acceptance_criteria.empty ())
        loop.AddMember (
          "acceptance_criteria",
          rapidjson::Value (acceptance_criteria.c_str (), a).Move (), a);
      doc.AddMember ("loop", loop, a);
    }

    return doc;
  }

  rapidjson::Document build_job_status_params (const std::string &job_id)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    doc.AddMember ("job_id", rapidjson::Value (job_id.c_str (), a).Move (), a);
    return doc;
  }

  rapidjson::Document build_job_list_params (const std::string &phase,
                                             const std::string &type_filter,
                                             int limit, int offset)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    if (!phase.empty ())
      doc.AddMember ("phase", rapidjson::Value (phase.c_str (), a).Move (), a);
    if (!type_filter.empty ())
      doc.AddMember ("type", rapidjson::Value (type_filter.c_str (), a).Move (),
                     a);
    doc.AddMember ("limit", limit, a);
    doc.AddMember ("offset", offset, a);
    return doc;
  }

  rapidjson::Document build_job_cancel_params (const std::string &job_id,
                                               bool stop_schedule)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    doc.AddMember ("job_id", rapidjson::Value (job_id.c_str (), a).Move (), a);
    doc.AddMember ("stop_schedule", stop_schedule, a);
    return doc;
  }

  // ---------------------------------------------------------------------------
  // Worker
  // ---------------------------------------------------------------------------

  rapidjson::Document build_worker_register_params (const std::string &path)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    doc.AddMember ("path", rapidjson::Value (path.c_str (), a).Move (), a);
    return doc;
  }

  rapidjson::Document build_worker_list_params (const std::string &enabled_str)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    if (!enabled_str.empty ())
      doc.AddMember ("enabled",
                     rapidjson::Value (enabled_str.c_str (), a).Move (), a);
    return doc;
  }

  rapidjson::Document build_worker_toggle_params (const std::string &worker_id)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    doc.AddMember ("worker_id",
                   rapidjson::Value (worker_id.c_str (), a).Move (), a);
    return doc;
  }

  // ---------------------------------------------------------------------------
  // Adviser
  // ---------------------------------------------------------------------------

  rapidjson::Document build_adviser_register_params (const std::string &path)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    doc.AddMember ("path", rapidjson::Value (path.c_str (), a).Move (), a);
    return doc;
  }

  rapidjson::Document build_adviser_list_params ()
  {
    return rapidjson::Document (rapidjson::kObjectType);
  }

  // ---------------------------------------------------------------------------
  // Review
  // ---------------------------------------------------------------------------

  rapidjson::Document build_review_list_params (const std::string &status,
                                                const std::string &type)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    if (!status.empty ())
      doc.AddMember ("status", rapidjson::Value (status.c_str (), a).Move (),
                     a);
    if (!type.empty ())
      doc.AddMember ("type", rapidjson::Value (type.c_str (), a).Move (), a);
    return doc;
  }

  rapidjson::Document build_review_id_params (const std::string &review_id)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    doc.AddMember ("review_id",
                   rapidjson::Value (review_id.c_str (), a).Move (), a);
    return doc;
  }

  rapidjson::Document build_review_reject_params (const std::string &review_id,
                                                  const std::string &message)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    doc.AddMember ("review_id",
                   rapidjson::Value (review_id.c_str (), a).Move (), a);
    doc.AddMember ("message", rapidjson::Value (message.c_str (), a).Move (),
                   a);
    return doc;
  }

  // ---------------------------------------------------------------------------
  // Forge
  // ---------------------------------------------------------------------------

  rapidjson::Document build_forge_list_params (const std::string &phase)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    if (!phase.empty ())
      doc.AddMember ("phase", rapidjson::Value (phase.c_str (), a).Move (), a);
    return doc;
  }

  rapidjson::Document build_forge_status_params (const std::string &forge_id)
  {
    rapidjson::Document doc (rapidjson::kObjectType);
    auto &a = doc.GetAllocator ();
    doc.AddMember ("forge_id", rapidjson::Value (forge_id.c_str (), a).Move (),
                   a);
    return doc;
  }

} // namespace agentos::cli
