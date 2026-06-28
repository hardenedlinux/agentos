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

#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/cli_format.h"
#include "agentos/job_params.h"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Option-bound variables MUST outlive register_job_commands() because CLI11
// stores the binding by reference and writes to it during app.parse() in
// main(). Local variables (whether at function scope or inside a `{ }` block)
// are destroyed when the function returns, leaving CLI11 with a dangling
// reference that it then writes to during parse — pure UB, observed in
// practice as ZMQ getting fed a garbage 8-byte sun_path for set_socket_path
// and looping ENOENT forever, hanging the CLI.
//
// Fix: every option-bound variable goes into a shared_ptr. CLI11 binds to
// the heap object; callbacks capture the shared_ptr by value so the heap
// object stays alive as long as any callback (or the main app) might fire.
// ---------------------------------------------------------------------------

namespace
{

  void print_json (const rapidjson::Document &doc)
  {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    doc.Accept (w);
    std::cout << buf.GetString () << "\n";
  }

} // unnamed namespace

void register_job_commands (CLI::App &app)
{
  auto *job = app.add_subcommand ("job", "Manage jobs");
  job->require_subcommand (1);

  // ---- shared options on `job` (visible to every subcommand) ----
  auto home = std::filesystem::path (std::getenv ("HOME"));

  auto timeout_ms = std::make_shared<int> (5000);
  auto socket_path = std::make_shared<std::string> ();
  auto json_flag = std::make_shared<bool> (false);
  auto access_key = std::make_shared<std::string> ();

  job->add_option ("--timeout", *timeout_ms)->default_val (5000);
  job->add_option ("--socket", *socket_path)
    ->default_val ((home / ".agentos/run/agentos.sock").string ());
  job->add_flag ("--json", *json_flag);
  job->add_option ("--key", *access_key,
                   "Access key (64-char hex); defaults to first active key in DB");

  // ---- job submit ----
  {
    auto *submit = job->add_subcommand ("submit", "Submit a new job");

    auto goal = std::make_shared<std::string> ();
    auto input_str = std::make_shared<std::string> ();
    auto type = std::make_shared<std::string> ("oneshot");
    auto interval_s = std::make_shared<int64_t> (0);
    auto starts_at = std::make_shared<int64_t> (0);
    auto max_iterations = std::make_shared<int> (5);
    auto reviewer_id = std::make_shared<std::string> ();
    auto acceptance_criteria = std::make_shared<std::string> ();
    auto user_id = std::make_shared<std::string> ("0");

    submit->add_option ("--goal", *goal)->required ();
    submit->add_option ("--input", *input_str);
    submit->add_option ("--type", *type)->default_val ("oneshot");
    submit->add_option ("--interval", *interval_s);
    submit->add_option ("--starts-at", *starts_at);
    submit->add_option ("--max-iterations", *max_iterations)->default_val (5);
    submit->add_option ("--reviewer", *reviewer_id);
    submit->add_option ("--acceptance-criteria", *acceptance_criteria);
    submit->add_option ("--user", *user_id)->default_val ("0");

    submit->callback (
      [timeout_ms, socket_path, json_flag, access_key, goal, input_str, type, interval_s,
       starts_at, max_iterations, reviewer_id, acceptance_criteria, user_id]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!access_key->empty ())
            client.set_access_key (*access_key);

          if (!socket_path->empty ())
          {
            spdlog::info ("socket_path is {}", *socket_path);
            client.set_socket_path (*socket_path);
          }
          else
          {
            spdlog::error ("socket_path is unknown!");
            std::exit (-1);
          }
          auto params = agentos::cli::build_job_submit_params (
            *goal, *type, *input_str, *interval_s, *starts_at, *max_iterations,
            *reviewer_id, *acceptance_criteria);
          {
            using rapidjson::Value;
            auto &alloc = params.GetAllocator ();
            params.AddMember ("user_id", Value (user_id->c_str (), alloc),
                              alloc);
          }
          auto result = client.send ("job.submit", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            std::cout << "job_id: " << result["job_id"].GetString () << "\n";
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (submit);
  }

  // ---- job status ----
  {
    auto *status = job->add_subcommand ("status", "Get job status");
    auto job_id = std::make_shared<std::string> ();
    status->add_option ("job_id", *job_id)->required ();

    status->callback (
      [timeout_ms, socket_path, json_flag, access_key, job_id]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!access_key->empty ())
            client.set_access_key (*access_key);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          auto params = agentos::cli::build_job_status_params (*job_id);
          auto result = client.send ("job.status", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            using namespace agentos::cli::color;
            namespace f = agentos::cli::fmt;
            if (!result.HasMember ("job_id") || !result["job_id"].IsString ())
            {
              std::cerr << "invalid response\n";
              return;
            }

            std::string id    = f::str (result, "job_id");
            std::string phase = f::str (result, "phase");
            std::string goal  = f::str (result, "goal");

            std::string phaseColored;
            if (phase == "executing" || phase == "repairing")
              phaseColored = yellow (phase);
            else if (phase == "done")
              phaseColored = green (phase);
            else if (phase == "failed" || phase == "human_review")
              phaseColored = red (phase);
            else if (phase == "planning")
              phaseColored = cyan (phase);
            else if (phase == "cancelled")
              phaseColored = grey (phase);
            else
              phaseColored = phase;

            std::cout << "Job " << id << "  [" << phaseColored << "]\n";
            std::cout << "Goal: " << goal << "\n";
            std::cout << "Created: " << f::ts (result, "created_at")
                      << "   Updated: " << f::ts (result, "updated_at") << "\n";

            if (result.HasMember ("steps") && result["steps"].IsArray ())
            {
              std::cout << "\nSteps:\n";

              auto now_s = static_cast<int64_t> (
                std::chrono::duration_cast<std::chrono::seconds> (
                  std::chrono::system_clock::now ().time_since_epoch ())
                  .count ());

              int64_t job_created = (result.HasMember ("created_at")
                                     && result["created_at"].IsInt64 ())
                                      ? result["created_at"].GetInt64 () : 0;
              int64_t job_total_s = 0;

              const auto &steps_arr = result["steps"].GetArray ();
              for (rapidjson::SizeType si = 0; si < steps_arr.Size (); ++si)
              {
                const auto &step = steps_arr[si];
                int order = step.HasMember ("step_order")
                              ? step["step_order"].GetInt () : -1;
                std::string desc   = f::str (step, "description");
                std::string stepStatus = f::str (step, "status");

                int64_t queued_at    = (step.HasMember ("queued_at")
                                        && step["queued_at"].IsInt64 ())
                                         ? step["queued_at"].GetInt64 () : 0;
                int64_t started_at   = (step.HasMember ("started_at")
                                        && step["started_at"].IsInt64 ())
                                         ? step["started_at"].GetInt64 () : 0;
                int64_t completed_at = (step.HasMember ("completed_at")
                                        && step["completed_at"].IsInt64 ())
                                         ? step["completed_at"].GetInt64 () : 0;

                // pending duration: from job created (or prev step done) to queued
                // planning duration: from queued to started (LLM + forge wait)
                // running duration:  from started to completed
                // For step 0 use job_created as the reference point.
                int64_t prev_completed = (si == 0) ? job_created : 0;
                if (si > 0)
                {
                  const auto &prev = steps_arr[si - 1];
                  if (prev.HasMember ("completed_at")
                      && prev["completed_at"].IsInt64 ())
                    prev_completed = prev["completed_at"].GetInt64 ();
                }

                std::string colStatus;
                if (stepStatus == "done")
                  colStatus = green (stepStatus);
                else if (stepStatus == "running")
                  colStatus = yellow (stepStatus);
                else if (stepStatus == "failed")
                  colStatus = red (stepStatus);
                else
                  colStatus = grey (stepStatus);

                std::cout << "\n  " << order << "  "
                          << colStatus << "  " << desc << "\n";

                // pending line: time between prev step done and this step queued
                if (queued_at > 0 && prev_completed > 0
                    && queued_at > prev_completed)
                {
                  int64_t s = queued_at - prev_completed;
                  std::cout << "       " << grey ("pending   ")
                            << std::to_string (s) << "s\n";
                }

                // planning line: queued → started (LLM planning + forge)
                if (queued_at > 0 && started_at > 0 && started_at >= queued_at)
                {
                  int64_t s = started_at - queued_at;
                  std::cout << "       " << grey ("planning  ")
                            << std::to_string (s) << "s\n";
                }
                else if (queued_at == 0 && prev_completed > 0
                         && started_at > prev_completed)
                {
                  // queued_at not available — use prev_completed as proxy
                  int64_t s = started_at - prev_completed;
                  std::cout << "       " << grey ("planning  ")
                            << std::to_string (s) << "s\n";
                }

                // running line
                if (started_at > 0)
                {
                  int64_t end = (completed_at > 0) ? completed_at : now_s;
                  int64_t s = end - started_at;
                  std::string label = (completed_at > 0)
                                        ? "running   " : "running   ";
                  std::cout << "       " << grey (label)
                            << std::to_string (s) << "s\n";
                }

                // step total
                if (prev_completed > 0 && completed_at > 0
                    && completed_at >= prev_completed)
                {
                  int64_t s = completed_at - prev_completed;
                  job_total_s += s;
                  std::cout << "       " << grey ("total     ")
                            << std::to_string (s) << "s\n";
                }
              }

              // Job total
              if (job_total_s > 0)
                std::cout << "\n" << grey ("Job total: "
                            + std::to_string (job_total_s) + "s") << "\n";
            

            // Show result for done jobs.
            if (phase == "done" && result.HasMember ("result_json")
                && result["result_json"].IsString ())
            {
              std::string rj = result["result_json"].GetString ();
              if (!rj.empty () && rj != "null")
                std::cout << "\nResult:\n" << rj << "\n";
            }
            }

            if (result.HasMember ("loop") && result["loop"].IsObject ())
            {
              const auto &loop = result["loop"];
              int curIter = loop.HasMember ("current_iteration")
                              ? loop["current_iteration"].GetInt ()
                              : 0;
              int maxIter = loop.HasMember ("max_iterations")
                              ? loop["max_iterations"].GetInt ()
                              : 0;
              int repairs = loop.HasMember ("current_repairs")
                              ? loop["current_repairs"].GetInt ()
                              : 0;
              std::cout << "Iteration: " << curIter << "/" << maxIter
                        << "   Repairs: " << repairs << "\n";
              if (loop.HasMember ("last_feedback")
                  && loop["last_feedback"].IsString ())
              {
                std::cout << "Last feedback: "
                          << loop["last_feedback"].GetString () << "\n";
              }
            }
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (status);
  }

  // ---- job list ----
  {
    auto *list = job->add_subcommand ("list", "List jobs");
    spdlog::set_level (spdlog::level::info);

    auto phase = std::make_shared<std::string> ();
    auto type_filter = std::make_shared<std::string> ();
    auto limit = std::make_shared<int> (50);
    auto offset = std::make_shared<int> (0);
    auto user_filter = std::make_shared<std::string> ();
    auto show_user = std::make_shared<bool> (false);
    auto all_jobs = std::make_shared<bool> (false);
    auto since_minutes = std::make_shared<int> (0);

    list->add_option ("--phase", *phase);
    list->add_option ("--type", *type_filter);
    list->add_option ("--limit", *limit)->default_val (50);
    list->add_option ("--offset", *offset)->default_val (0);
    list->add_option ("--user", *user_filter, "Filter by user_id");
    list->add_flag ("--show-user", *show_user, "Show user_id column in table");
    list->add_flag ("--all", *all_jobs, "Show all jobs (no time limit)");
    list->add_option ("--since", *since_minutes, "Show jobs updated in last N minutes (default: 10)");

    list->callback (
      [timeout_ms, socket_path, json_flag, access_key, phase, type_filter,
       limit, offset, user_filter, show_user, all_jobs, since_minutes]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!access_key->empty ())
            client.set_access_key (*access_key);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          else
          {
            spdlog::error ("socket_path is unknown!");
            std::exit (-1);
          }

          auto params = agentos::cli::build_job_list_params (
            *phase, *type_filter, *limit, *offset);
          if (!user_filter->empty ())
            params.AddMember ("user_id",
                              rapidjson::Value (user_filter->c_str (),
                                               params.GetAllocator ()),
                              params.GetAllocator ());
          if (*all_jobs)
            params.AddMember ("all", rapidjson::Value (true),
                              params.GetAllocator ());
          else if (*since_minutes > 0)
            params.AddMember ("since_minutes",
                              rapidjson::Value (*since_minutes),
                              params.GetAllocator ());
          auto result = client.send ("job.list", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            using namespace agentos::cli::color;
            namespace f = agentos::cli::fmt;
            if (!result.HasMember ("jobs") || !result["jobs"].IsArray ())
            {
              std::cout << "No jobs.\n";
              return;
            }
            const auto &jobs = result["jobs"];
            struct Row
            {
              std::string id, type, phase, goal, created, user_id;
            };
            std::vector<Row> rows;
            for (const auto &j : jobs.GetArray ())
            {
              std::string id = f::str (j, "id");
              std::string type = f::str (j, "type");
              std::string phase = f::str (j, "phase");
              std::string goal = f::str (j, "goal");
              if (goal.size () > 40)
                goal = goal.substr (0, 40) + "...";
              std::string created = f::ts (j, "created_at");
              std::string uid = f::str (j, "user_id");
              rows.push_back ({id, type, phase, goal, created, uid});
            }

            size_t w_id = 2, w_type = 4, w_phase = 5, w_goal = 5,
                   w_created = 7, w_user = 4;
            for (const auto &r : rows)
            {
              w_id = std::max (w_id, r.id.size ());
              w_type = std::max (w_type, r.type.size ());
              w_phase = std::max (w_phase, r.phase.size ());
              w_goal = std::max (w_goal, r.goal.size ());
              w_created = std::max (w_created, r.created.size ());
              if (*show_user)
                w_user = std::max (w_user, r.user_id.size ());
            }

            size_t total_w = w_id + w_type + w_phase + w_goal + w_created + 8;
            if (*show_user)
              total_w += w_user + 2;

            auto phase_color = [&] (const std::string &p) -> std::string
            {
              if (p == "executing" || p == "repairing")
                return yellow (p);
              if (p == "done")
                return green (p);
              if (p == "failed" || p == "human_review")
                return red (p);
              if (p == "planning")
                return cyan (p);
              if (p == "cancelled")
                return grey (p);
              return p;
            };

            std::cout << bold (f::col ("ID", w_id))
                      << bold (f::col ("TYPE", w_type))
                      << bold (f::col ("PHASE", w_phase))
                      << bold (f::col ("GOAL", w_goal));
            if (*show_user)
              std::cout << bold (f::col ("USER", w_user));
            std::cout << bold ("CREATED") << "\n";
            std::cout << f::separator (total_w) << "\n";

            for (const auto &r : rows)
            {
              std::cout << f::col (r.id, w_id) << f::col (r.type, w_type)
                        << f::col_colored (phase_color (r.phase), r.phase,
                                           w_phase)
                        << f::col (r.goal, w_goal);
              if (*show_user)
                std::cout << f::col (r.user_id, w_user);
              std::cout << r.created << "\n";
            }

            int total = (result.HasMember ("total") && result["total"].IsInt ())
                          ? result["total"].GetInt ()
                          : static_cast<int> (rows.size ());
            std::cout << "\ntotal: " << total << " jobs\n";
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (list);
  }

  // ---- job cancel ----
  {
    auto *cancel = job->add_subcommand ("cancel", "Cancel a job");
    auto job_id = std::make_shared<std::string> ();
    auto keep_schedule = std::make_shared<bool> (false);
    cancel->add_option ("job_id", *job_id)->required ();
    cancel->add_flag ("--keep-schedule", *keep_schedule);

    cancel->callback (
      [timeout_ms, socket_path, json_flag, access_key, job_id, keep_schedule]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!access_key->empty ())
            client.set_access_key (*access_key);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          auto params
            = agentos::cli::build_job_cancel_params (*job_id, !*keep_schedule);
          auto result = client.send ("job.cancel", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            std::cout << "cancelled: " << *job_id << "\n";
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (cancel);
  }

  agentos::cli::add_completion (job);
}
