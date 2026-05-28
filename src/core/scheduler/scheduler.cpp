#include "agentos/scheduler.h"
#include "agentos/capability.h"
#include "agentos/registry.h"
#include "agentos/rpc.h"
#include "agentos/database/database.h"
#include "agentos/sandbox.h"
#include <regex>
#include <spdlog/spdlog.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <chrono>

namespace agentos
{

  Scheduler::Scheduler (const Registry &registry, Dispatcher &dispatcher,
                        const SchedulerConfig &config)
    : registry_ (registry), dispatcher_ (dispatcher),
      config_ (config)
  {
  }

  TaskResult Scheduler::run (const Plan &plan)
  {
    // TODO Phase 1: full topological sort + parallel execution via libuv thread
    // pool
    //
    // Current stub: sequential execution in depends_on order (no parallelism)
    //
    // Phase 1 algorithm:
    //   1. Build adjacency list from depends_on
    //   2. Topological sort (Kahn's)
    //   3. Group steps with no unmet deps into a "ready" queue
    //   4. Dispatch ready steps in parallel via uv_queue_work
    //   5. On each completion, unlock downstream steps and re-enqueue
    //   6. Collect all results into StepResultMap
    //   7. On any failure: cancel pending steps, surface error

    spdlog::info ("[scheduler] running plan {} ({} steps)", plan.task_id,
                  plan.steps.size ());

    StepResultMap results;

    // ADR-016: Generate run_id for this plan execution
    std::string run_id = gen_new_uuid();
    spdlog::info("[scheduler] run_id={} for plan {}", run_id, plan.task_id);

    // Record worker run in DB (if available)
    // We'll use a dummy worker_id for now; actual worker_id should come from step.
    // For simplicity, we use plan.task_id as worker_id.
    // The DB is accessed via registry_? Actually we need a Database reference.
    // We'll assume the Scheduler has access to a Database via some member.
    // For now, we'll skip DB recording because Scheduler doesn't have a Database member.
    // TODO: add Database reference to Scheduler.

    for (const auto &step : plan.steps)
    {
      // ADR-006 Layer 2: validate capabilities for generated code
      if (!validate_step_capabilities(step))
      {
        return TaskResult{plan.task_id, false, "",
                          "capability validation failed for step " + step.id};
      }

      spdlog::info ("[scheduler] executing step '{}' command '{}'", step.id,
                    step.command);

      auto worker = registry_.find_worker_for_command (step.command);
      if (!worker)
      {
        return TaskResult{plan.task_id, false, "",
                          "no worker for command: " + step.command};
      }

      // Interpolate {{step.field}} variable references
      std::string args_json
      = "{}"; // TODO: serialise step.args with interpolation
      // args_json = interpolate_args(step.args_json, results);

      // Build task JSON for the worker, including run_id
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w(buf);
      w.StartObject();
      w.Key("job_id");   w.String(plan.task_id.value().c_str());
      w.Key("task_id");  w.String(step.id.c_str());
      w.Key("method");   w.String(step.command.c_str());
      w.Key("run_id");   w.String(run_id.c_str());
      w.Key("params");
      w.RawValue(args_json.c_str(), args_json.size(), rapidjson::kObjectType);
      w.EndObject();
      std::string task_json = buf.GetString();

      // Create per-task PUSH socket and send task
      std::string push_path = dispatcher_.create_task_push(step.id);
      if (push_path.empty())
      {
        return TaskResult{plan.task_id, false, "",
                          "failed to create PUSH socket for step " + step.id};
      }

      std::string result_json;
      for (int attempt = 0; attempt <= config_.max_retries; ++attempt)
      {
        dispatcher_.send_task(step.id, task_json);
        // Wait for result (blocking)
        result_json = dispatcher_.receive_result();
        if (!result_json.empty())
          break;
        spdlog::warn ("[scheduler] step '{}' attempt {} failed, retrying",
                      step.id, attempt + 1);
      }

      // Close per-task PUSH socket
      dispatcher_.close_task_push(step.id);

      results[step.id] = result_json;
      spdlog::info ("[scheduler] step '{}' done", step.id);
    }

    // ADR-016: After all steps, trigger GC (if DB available)
    // TODO: integrate with Database when Scheduler has access.

    // Assemble final output — last step's result is the task output
    std::string output = results.empty () ? "{}" : results.begin ()->second;
    return TaskResult{plan.task_id, true, output, ""};
  }

  bool Scheduler::validate_step_capabilities(const PlanStep &step)
  {
    // If the command is pre‑approved, no capability check needed.
    auto worker = registry_.find_worker_for_command(step.command);
    if (worker.has_value()) {
      return true;
    }

    if (!step.capabilities.has_value()) {
      spdlog::error("[scheduler] step '{}' command '{}' missing capability declaration",
                    step.id, step.command);
      return false;
    }

    const auto &cap = step.capabilities.value();
    if (!validate_capability(cap, "")) {
      spdlog::error("[scheduler] step '{}' command '{}' capability validation failed",
                    step.id, step.command);
      return false;
    }

    return true;
  }

  std::string Scheduler::interpolate_args (const std::string &args_template,
                                           const StepResultMap &results) const
  {
    // Replace {{step_id.field}} with the corresponding value from results
    // TODO Phase 1: proper JSON path resolution within result_json
    std::string out = args_template;
    static const std::regex ref_pattern (R"(\{\{(\w+)\.(\w+)\}\})");
    std::smatch m;
    std::string in = out;
    out.clear ();
    while (std::regex_search (in, m, ref_pattern))
    {
      out += m.prefix ().str ();
      const std::string step_id = m[1];
      // const std::string field   = m[2];  // TODO: extract field from result
      // JSON
      auto it = results.find (step_id);
      out += (it != results.end ()) ? it->second : "null";
      in = m.suffix ().str ();
    }
    out += in;
    return out;
  }

} // namespace agentos
