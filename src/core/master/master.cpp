/**
 * agentos/master.cpp
 *
 * ADR-002: Master is the sole decision-maker
 * ADR-009: Mind Layer — LLM reasoning; Enforce Layer lives in Orchestrator
 * ADR-012: LLM calls are synchronous, non-streaming
 */
#include "agentos/master.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>

namespace agentos
{

  Master::Master (LlmClient &llm, Orchestrator &orchestrator,
                  Registry &registry)
    : llm_ (llm), orchestrator_ (orchestrator), registry_ (registry)
  {
  }

  TaskResult Master::submit (const Task &task)
  {
    spdlog::info ("[master] task submitted: {} goal='{}'", task.id, task.goal);

    // ── 1. Mind Layer: select adviser
    // ──────────────────────────────────────────
    const std::string adviser_id = select_adviser (task);
    if (adviser_id.empty ())
    {
      spdlog::error ("[master] LLM could not select an adviser for task {}",
                     task.id);
      return {task.id, false, "", "no adviser selected"};
    }
    spdlog::info ("[master] selected adviser: {}", adviser_id);

    // ── 2. Obtain plan from adviser (via Orchestrator)
    // ─────────────────────────
    auto plan = orchestrator_.request_plan (adviser_id, task);
    if (!plan)
    {
      spdlog::error ("[master] adviser {} failed to produce a plan",
                     adviser_id);
      return {task.id, false, "", "adviser failed to produce a plan"};
    }
    spdlog::info ("[master] plan received: {} steps", plan->steps.size ());

    // ── 3. Mind Layer: review plan
    // ─────────────────────────────────────────────
    const std::string rejection = review_plan (task, *plan);
    if (!rejection.empty ())
    {
      spdlog::error ("[master] plan review rejected: {}", rejection);
      return {task.id, false, "", "plan rejected by master: " + rejection};
    }
    spdlog::info ("[master] plan approved, delegating to orchestrator");

    // ── 4. Delegate execution to Orchestrator
    // ──────────────────────────────────
    return orchestrator_.execute (task, *plan);
  }

  // -----------------------------------------------------------------------------
  // Mind Layer: adviser selection
  // -----------------------------------------------------------------------------

  std::string Master::select_adviser (const Task &task)
  {
    // Build a list of available advisers for the LLM to choose from.
    const auto advisers = registry_.all_advisers ();
    if (advisers.empty ())
    {
      spdlog::error ("[master] no advisers registered");
      return "";
    }

    // If there is only one adviser, skip the LLM call.
    if (advisers.size () == 1)
      return advisers.front ().id;

    const std::string system_prompt = build_selection_prompt (task);

    LlmRequest req;
    req.system_prompt = system_prompt;
    req.user_prompt = task.goal;
    req.max_tokens = 256;

    auto result = llm_.complete (req);
    if (!result.ok)
    {
      spdlog::error ("[master] LLM call failed during adviser selection: {}",
                     result.error);
      return "";
    }

    // Expect the LLM to return a JSON object: { "adviser_id": "..." }
    rapidjson::Document doc;
    if (doc.Parse (result.value.content.c_str ()).HasParseError ()
        || !doc.IsObject () || !doc.HasMember ("adviser_id")
        || !doc["adviser_id"].IsString ())
    {
      spdlog::error ("[master] LLM returned unexpected adviser selection: {}",
                     result.value.content);
      return "";
    }

    return doc["adviser_id"].GetString ();
  }

  std::string Master::build_selection_prompt (const Task &task) const
  {
    const auto advisers = registry_.all_advisers ();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartArray ();
    for (const auto &a : advisers)
    {
      w.StartObject ();
      w.Key ("id");
      w.String (a.id.c_str ());
      w.Key ("name");
      w.String (a.name.c_str ());
      w.Key ("description");
      w.String (a.description.c_str ());
      w.EndObject ();
    }
    w.EndArray ();

    return std::string (
             "You are the Master of an agent orchestration system. "
             "Given a task goal and a list of available advisers, select the "
             "most appropriate adviser to plan the task. "
             "Respond with a JSON object containing only the field "
             "'adviser_id' "
             "with the id of the chosen adviser. "
             "Available advisers: ")
           + buf.GetString ();
  }

  // -----------------------------------------------------------------------------
  // Mind Layer: plan review
  // -----------------------------------------------------------------------------

  std::string Master::review_plan (const Task &task, const Plan &plan)
  {
    const std::string system_prompt = build_review_prompt (task, plan);

    LlmRequest req;
    req.system_prompt = system_prompt;
    req.user_prompt
      = "Review the plan above. Does it correctly address the task goal?";
    req.max_tokens = 512;

    auto result = llm_.complete (req);
    if (!result.ok)
    {
      spdlog::error ("[master] LLM call failed during plan review: {}",
                     result.error);
      // Fail open: if LLM is unavailable, let the plan through rather than
      // blocking all tasks. Log the failure clearly.
      return "";
    }

    // Expect: { "approved": true } or { "approved": false, "reason": "..." }
    rapidjson::Document doc;
    if (doc.Parse (result.value.content.c_str ()).HasParseError ()
        || !doc.IsObject () || !doc.HasMember ("approved")
        || !doc["approved"].IsBool ())
    {
      spdlog::warn ("[master] LLM returned unexpected plan review response: {}",
                    result.value.content);
      return "";
    }

    if (doc["approved"].GetBool ())
      return "";

    if (doc.HasMember ("reason") && doc["reason"].IsString ())
      return doc["reason"].GetString ();

    return "plan rejected by LLM review (no reason given)";
  }

  std::string Master::build_review_prompt (const Task &task,
                                           const Plan &plan) const
  {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("goal");
    w.String (task.goal.c_str ());
    w.Key ("steps");
    w.StartArray ();
    for (const auto &step : plan.steps)
    {
      w.StartObject ();
      w.Key ("id");
      w.String (step.id.c_str ());
      w.Key ("command");
      w.String (step.command.c_str ());
      w.Key ("depends_on");
      w.StartArray ();
      for (const auto &d : step.depends_on)
        w.String (d.c_str ());
      w.EndArray ();
      w.EndObject ();
    }
    w.EndArray ();
    w.EndObject ();

    return std::string (
                        "You are the Master of an agent orchestration system reviewing a "
                        "plan produced by an Adviser. "
                        "The plan must correctly address the task goal using only the "
                        "declared steps and their dependencies. "
                        "Respond with a JSON object: "
                        "{ \"approved\": true } if the plan is sound, or "
                        "{ \"approved\": false, \"reason\": \"...\" } if it is not. "
                        "Task and plan: ")
      + buf.GetString ();
  }

} // namespace agentos
