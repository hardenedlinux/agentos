#include "agentos/scheduler.h"
#include "agentos/capability.h"
#include "agentos/database/database.h"
#include "agentos/home_init.h"
#include "agentos/registry.h"
#include "agentos/rpc.h"
#include "agentos/sandbox.h"
#include <chrono>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <regex>
#include <spdlog/spdlog.h>

namespace agentos
{

  Scheduler::Scheduler (const Registry &registry, Dispatcher &dispatcher,
                        const SchedulerConfig &config, Database &db)
    : registry_ (registry), dispatcher_ (dispatcher), config_ (config), db_ (db)
  {
  }

  TaskResult Scheduler::run (const Plan &plan)
  {
    spdlog::info ("[scheduler] running plan {} ({} steps)", plan.task_id,
                  plan.steps.size ());

    StepResultMap results;

    // ADR-016: Generate run_id for this plan execution.
    // run_id identifies the execution record in worker_runs; it is generated
    // here before any step executes and recorded in the DB for traceability.
    const std::string run_id = gen_new_uuid ();
    spdlog::info ("[scheduler] run_id={} for plan {}", run_id, plan.task_id);

    // Record the plan execution in worker_runs (ADR-016).
    // worker_id is not yet known at plan level; recorded per step below.
    // The run record is opened here and closed after all steps complete.
    db_.record_run_started (run_id, plan.task_id.value ());

    for (const auto &step : plan.steps)
    {
      // Resolve the worker for this step.
      auto worker = registry_.find_worker_for_command (step.command);
      if (!worker)
      {
        const std::string err = "no worker for command: " + step.command;
        spdlog::error ("[scheduler] step '{}': {}", step.id, err);
        db_.record_run_failed (run_id, err);
        return TaskResult{plan.task_id, false, "", err};
      }

      // ADR-006 Layer 2 / ADR-009 Enforce Layer:
      // Validate capabilities before every spawn, for all workers regardless
      // of tier. No worker is unconditionally trusted.
      if (auto err = validate_step_capabilities (step, worker->worker_id);
          !err.empty ())
      {
        spdlog::error ("[scheduler] step '{}': {}", step.id, err);
        db_.record_run_failed (run_id, err);
        return TaskResult{plan.task_id, false, "",
                          "capability validation failed for step " + step.id
                            + ": " + err};
      }

      spdlog::info ("[scheduler] executing step '{}' command '{}'", step.id,
                    step.command);

      // Interpolate {{step_id.field}} references in args.
      std::string args_json = interpolate_args (step.args, results);

      // Build task JSON for the worker (ADR-003 wire format).
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      w.StartObject ();
      w.Key ("job_id");
      w.String (plan.task_id.value ().c_str ());
      w.Key ("task_id");
      w.String (step.id.c_str ());
      w.Key ("method");
      w.String (step.command.c_str ());
      w.Key ("run_id");
      w.String (run_id.c_str ());
      w.Key ("params");
      w.RawValue (args_json.c_str (), args_json.size (),
                  rapidjson::kObjectType);
      w.EndObject ();
      const std::string task_json = buf.GetString ();

      // Create per-task PUSH socket and dispatch (ADR-003).
      const std::string push_path = dispatcher_.create_task_push (step.id);
      if (push_path.empty ())
      {
        const std::string err
          = "failed to create PUSH socket for step " + step.id;
        db_.record_run_failed (run_id, err);
        return TaskResult{plan.task_id, false, "", err};
      }

      std::string result_json;
      for (int attempt = 0; attempt <= config_.max_retries; ++attempt)
      {
        dispatcher_.send_task (step.id, task_json);
        result_json = dispatcher_.receive_result ();
        if (!result_json.empty ())
          break;
        spdlog::warn ("[scheduler] step '{}' attempt {} failed, retrying",
                      step.id, attempt + 1);
      }

      dispatcher_.close_task_push (step.id);

      if (result_json.empty ())
      {
        const std::string err = "step " + step.id + " failed after all retries";
        db_.record_run_failed (run_id, err);
        return TaskResult{plan.task_id, false, "", err};
      }

      results[step.id] = result_json;
      spdlog::info ("[scheduler] step '{}' done", step.id);
    }

    // All steps completed; close the run record.
    db_.record_run_completed (run_id);

    // ADR-016: GC of ephemeral run layers is triggered asynchronously by the
    // GC component after run completion; Scheduler does not call GC directly.

    // Final output is the last step's result.
    const std::string output
      = results.empty () ? "{}" : results.rbegin ()->second;
    return TaskResult{plan.task_id, true, output, ""};
  }

  /**
   * Validate capabilities for a single plan step before spawn (ADR-006,
   * ADR-009).
   *
   * Called for every worker, every execution — no unconditional trust
   * (ADR-009). worker_dir is the worker's static registered directory
   * (~/.agentos/workers/<worker-id>/), established at registration time
   * (ADR-016).
   *
   * Returns an empty string on success, or an error/reason string on failure.
   * The caller logs and propagates the returned string.
   */
  std::string
  Scheduler::validate_step_capabilities (const PlanStep &step,
                                         const std::string &worker_id)
  {
    if (!step.capabilities.has_value ())
    {
      // A missing capability declaration is a registration defect; reject.
      return "step '" + step.id + "' command '" + step.command
             + "' is missing a capability declaration";
    }

    const auto &cap = step.capabilities.value ();
    const auto home = agentos_home ();
    const fs::path worker_dir = home / "workers" / worker_id;

    auto result = validate_capability (cap, worker_dir);

    if (!result.has_value ())
    {
      // Caller contract violation (e.g. unsubstituted placeholder).
      return result.error ();
    }

    switch (result->verdict)
    {
    case CapabilityVerdict::Approve:
      return "";

    case CapabilityVerdict::Reject:
      return "capability rejected for step '" + step.id
             + "': " + result->reason;

    case CapabilityVerdict::Escalate:
      // Insert into human_reviews and surface as a failure for this run.
      // The human_reviews record allows an operator to approve the path
      // access and re-run (ADR-006, ADR-008).
      db_.insert_human_review (step.id, result->reason);
      return "capability escalated for step '" + step.id
             + "' (pending human review): " + result->reason;
    }

    // Unreachable; silence compiler warning.
    return "unknown capability verdict";
  }

  std::string Scheduler::interpolate_args (
    const std::unordered_map<std::string, std::string> &args,
    const StepResultMap &results) const
  {
    static const std::regex ref_pattern (R"(\{\{(\w+)\.(\w+)\}\})");

    // Resolve a single value string, replacing all {{step_id.field}} refs.
    // ADR-003 result envelope: { "job_id", "task_id", "success", "result": {} }
    // Field references resolve against the "result" sub-object.
    auto resolve_value = [&] (const std::string &val) -> std::string
    {
      std::string out;
      std::string in = val;
      std::smatch m;

      while (std::regex_search (in, m, ref_pattern))
      {
        out += m.prefix ().str ();

        const std::string &step_id = m[1];
        const std::string &field = m[2];

        auto it = results.find (step_id);
        if (it == results.end ())
        {
          spdlog::warn ("[scheduler] interpolate: step '{}' not in results, "
                        "substituting null",
                        step_id);
          out += "null";
        }
        else
        {
          rapidjson::Document doc;
          doc.Parse (it->second.c_str ());

          if (doc.HasParseError () || !doc.IsObject ()
              || !doc.HasMember ("result") || !doc["result"].IsObject ())
          {
            spdlog::warn ("[scheduler] interpolate: step '{}' result is "
                          "not a valid envelope, substituting null",
                          step_id);
            out += "null";
          }
          else
          {
            const auto &result_obj = doc["result"];
            if (!result_obj.HasMember (field.c_str ()))
            {
              spdlog::warn ("[scheduler] interpolate: field '{}' not "
                            "found in step '{}' result, substituting null",
                            field, step_id);
              out += "null";
            }
            else
            {
              const auto &v = result_obj[field.c_str ()];
              if (v.IsString ())
              {
                out += v.GetString ();
              }
              else
              {
                rapidjson::StringBuffer sb;
                rapidjson::Writer<rapidjson::StringBuffer> w (sb);
                v.Accept (w);
                out += sb.GetString ();
              }
            }
          }
        }

        in = m.suffix ().str ();
      }

      out += in;
      return out;
    };

    // Serialise args map into a JSON object with all placeholders resolved.
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    for (const auto &[key, val] : args)
    {
      w.Key (key.c_str ());
      const std::string resolved = resolve_value (val);
      w.String (resolved.c_str ());
    }
    w.EndObject ();
    return buf.GetString ();
  }
} // namespace agentos
