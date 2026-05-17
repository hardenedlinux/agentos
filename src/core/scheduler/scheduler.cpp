#include "agentos/scheduler.h"
#include "agentos/registry.h"
#include <regex>
#include <spdlog/spdlog.h>

namespace agentos
{

  Scheduler::Scheduler (const Registry &registry, ExecuteFn execute_fn,
                        const SchedulerConfig &config)
    : registry_ (registry), execute_fn_ (std::move (execute_fn)),
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

    for (const auto &step : plan.steps)
    {
      spdlog::info ("[scheduler] executing step '{}' command '{}'", step.id,
                    step.command);

      auto executor = registry_.find_executor_for_command (step.command);
      if (!executor)
      {
        return TaskResult{plan.task_id, false, "",
                          "no executor for command: " + step.command};
      }

      // Interpolate {{step.field}} variable references
      std::string args_json
      = "{}"; // TODO: serialise step.args with interpolation
      // args_json = interpolate_args(step.args_json, results);

      std::string result_json;
      for (int attempt = 0; attempt <= config_.max_retries; ++attempt)
        {
          result_json = execute_fn_ (executor->id, step.command, args_json);
          if (!result_json.empty ())
            break;
          spdlog::warn ("[scheduler] step '{}' attempt {} failed, retrying",
                        step.id, attempt + 1);
        }

      results[step.id] = result_json;
      spdlog::info ("[scheduler] step '{}' done", step.id);
    }

    // Assemble final output — last step's result is the task output
    std::string output = results.empty () ? "{}" : results.begin ()->second;
    return TaskResult{plan.task_id, true, output, ""};
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
