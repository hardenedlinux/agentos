/**
 * agentos/orchestrator.cpp
 *
 * ADR-002: Orchestrator is the Enforce Layer execution arm of Master
 * ADR-005: Job persistence and crash recovery
 * ADR-009: No LLM calls here; all logic is deterministic
 */
#include "agentos/orchestrator.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>

namespace agentos
{

  Orchestrator::Orchestrator (Registry &registry, Verifier &verifier,
                              Scheduler &scheduler, Dispatcher &dispatcher,
                              Database &db)
    : registry_ (registry), verifier_ (verifier), scheduler_ (scheduler),
      dispatcher_ (dispatcher), db_ (db)
  {
  }

  // -----------------------------------------------------------------------------
  // Public interface
  // -----------------------------------------------------------------------------

  std::optional<Plan> Orchestrator::request_plan (const std::string &adviser_id,
                                                  const Task &task)
  {
    auto adviser = registry_.find_adviser_by_id (adviser_id);
    if (!adviser)
    {
      spdlog::error ("[orchestrator] adviser '{}' not found in registry",
                     adviser_id);
      return std::nullopt;
    }

    // Build planning request payload.
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("goal");
    w.String (task.goal.c_str ());
    w.Key ("input");
    w.RawValue (task.input_json.empty () ? "{}" : task.input_json.c_str (),
                task.input_json.empty () ? 2 : task.input_json.size (),
                rapidjson::kObjectType);
    w.Key ("available_commands");
    const std::string cmd_ctx = build_command_context ();
    w.RawValue (cmd_ctx.c_str (), cmd_ctx.size (), rapidjson::kArrayType);
    w.EndObject ();

    std::string plan_json;

    dispatcher_.send_request (
      adviser->id, "task.plan", buf.GetString (),
      [&] (const std::string &result, const std::string &error)
      {
        if (!error.empty ())
          spdlog::error ("[orchestrator] adviser '{}' returned error: {}",
                         adviser_id, error);
        else
          plan_json = result;
      });

    if (plan_json.empty ())
    {
      spdlog::error ("[orchestrator] adviser '{}' returned empty plan",
                     adviser_id);
      return std::nullopt;
    }

    return parse_plan (task.id, plan_json);
  }

  TaskResult Orchestrator::execute (const Task &task, const Plan &plan)
  {
    // ── 0. Persist job (phase = planning)
    // ──────────────────────────────────────
    store_job (task);

    // ── 1. Persist plan and tasks
    // ───────────────────────────────────────────────
    const std::string plan_json = serialize_plan (plan);
    update_job_plan (task.id, plan_json);
    for (const auto &step : plan.steps)
      store_task (task.id, step);

    // ── 2. Verify plan (Enforce Layer)
    // ──────────────────────────────────────────
    auto verify = verifier_.verify (plan);
    if (!verify.ok)
    {
      std::string errs;
      for (const auto &e : verify.errors)
        errs += "\n  - " + e;
      spdlog::error ("[orchestrator] plan verification failed:{}", errs);
      update_job_phase (task.id, "failed");
      return {task.id, false, "", "invalid plan: " + errs};
    }

    // ── 3. Execute via Scheduler
    // ────────────────────────────────────────────────
    update_job_phase (task.id, "executing");
    auto result = scheduler_.run (plan);
    update_job_phase (task.id, result.success ? "done" : "failed");
    return result;
  }

  void Orchestrator::resume_in_flight ()
  {
    // ADR-016: Mark any worker_runs with status='running' as crashed.
    db_.mark_all_running_as_crashed ();

    // ADR-005: Resume all jobs not in done/failed phase.
    auto jobs = db_.resume_in_flight ();
    for (const auto &j : jobs)
    {
      if (j.plan_json.empty ())
      {
        spdlog::warn ("[orchestrator] resume job {} has no plan, skipping",
                      j.job_id);
        continue;
      }

      auto plan = parse_plan (j.job_id, j.plan_json);
      if (!plan)
      {
        spdlog::warn (
          "[orchestrator] resume job {} failed to parse plan, skipping",
          j.job_id);
        continue;
      }

      spdlog::info ("[orchestrator] resuming job {} ({} steps)", j.job_id,
                    plan->steps.size ());

      auto verify = verifier_.verify (*plan);
      if (!verify.ok)
      {
        spdlog::error ("[orchestrator] resume job {} plan verification failed, "
                       "marking failed",
                       j.job_id);
        update_job_phase (j.job_id, "failed");
        continue;
      }

      auto result = scheduler_.run (*plan);
      update_job_phase (j.job_id, result.success ? "done" : "failed");
    }
  }

  // -----------------------------------------------------------------------------
  // Private helpers
  // -----------------------------------------------------------------------------

  void Orchestrator::store_job (const Task &task)
  {
    db_.store_job (task);
  }

  void Orchestrator::update_job_phase (const TaskId &id,
                                       const std::string &phase)
  {
    db_.update_job_phase (id, phase);
  }

  void Orchestrator::update_job_plan (const TaskId &id,
                                      const std::string &plan_json)
  {
    db_.update_job_plan (id, plan_json);
  }

  void Orchestrator::store_task (const TaskId &job_id, const PlanStep &step)
  {
    db_.store_task (job_id, step);
  }

  std::string Orchestrator::serialize_plan (const Plan &plan) const
  {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("steps");
    w.StartArray ();
    for (const auto &step : plan.steps)
    {
      w.StartObject ();
      w.Key ("id");
      w.String (step.id.c_str ());
      w.Key ("command");
      w.String (step.command.c_str ());
      w.Key ("args");
      w.StartObject ();
      for (const auto &[k, v] : step.args)
      {
        w.Key (k.c_str ());
        w.String (v.c_str ());
      }
      w.EndObject ();
      w.Key ("depends_on");
      w.StartArray ();
      for (const auto &d : step.depends_on)
        w.String (d.c_str ());
      w.EndArray ();
      w.EndObject ();
    }
    w.EndArray ();
    w.EndObject ();
    return buf.GetString ();
  }

  std::optional<Plan>
  Orchestrator::parse_plan (const TaskId &task_id,
                            const std::string &plan_json) const
  {
    rapidjson::Document doc;
    if (doc.Parse (plan_json.c_str ()).HasParseError ())
    {
      spdlog::error ("[orchestrator] failed to parse plan JSON");
      return std::nullopt;
    }

    if (!doc.HasMember ("steps") || !doc["steps"].IsArray ())
    {
      spdlog::error ("[orchestrator] plan JSON missing 'steps' array");
      return std::nullopt;
    }

    Plan plan;
    plan.task_id = task_id;

    for (const auto &s : doc["steps"].GetArray ())
    {
      PlanStep step;
      if (s.HasMember ("id"))
        step.id = s["id"].GetString ();
      if (s.HasMember ("command"))
        step.command = s["command"].GetString ();

      if (s.HasMember ("args") && s["args"].IsObject ())
        for (auto it = s["args"].MemberBegin (); it != s["args"].MemberEnd ();
             ++it)
          step.args[it->name.GetString ()] = it->value.GetString ();

      if (s.HasMember ("depends_on") && s["depends_on"].IsArray ())
        for (const auto &d : s["depends_on"].GetArray ())
          step.depends_on.push_back (d.GetString ());

      plan.steps.push_back (std::move (step));
    }

    return plan;
  }

  std::string Orchestrator::build_command_context () const
  {
    auto schemas = registry_.all_command_schemas ();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartArray ();
    for (const auto &cmd : schemas)
      {
        w.StartObject ();
        w.Key ("name");
        w.String (cmd.name.c_str ());
        w.Key ("description");
        w.String (cmd.description.c_str ());
        w.Key ("timeout_ms");
        w.Int (cmd.limits.timeout_ms);
        w.EndObject ();
      }
    w.EndArray ();
    return buf.GetString ();
  }

} // namespace agentos
