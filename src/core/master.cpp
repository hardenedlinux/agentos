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
 * agentos/master.cpp
 *
 * ADR-002: Master is the sole decision-maker (Mind Layer).
 * ADR-009: LLM calls here only; Enforce Layer in Orchestrator.
 * ADR-024: Actor model — on_message() never blocks.
 */

#include "agentos/master.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <thread>

namespace agentos
{

  // ---------------------------------------------------------------------------
  // Construction
  // ---------------------------------------------------------------------------

  Master::Master (LlmClient &llm, Registry &registry,
                  SendToOrchestrator send_to_orchestrator)
    : llm_ (llm), registry_ (registry),
      send_to_orchestrator_ (std::move (send_to_orchestrator))
  {
  }

  // ---------------------------------------------------------------------------
  // on_message — always returns immediately
  // ---------------------------------------------------------------------------

  void Master::on_message (MasterEvent msg)
  {
    // Check for internal result messages first.
    if (msg.kind == MasterEvent::Kind::ScheduledTask)
    {
      rapidjson::Document doc;
      if (!doc.Parse (msg.payload_json.c_str ()).HasParseError ()
          && doc.HasMember ("_internal") && doc["_internal"].IsString ())
      {
        const std::string internal = doc["_internal"].GetString ();
        if (internal == "adviser_selected")
        {
          handle_adviser_selected (msg.payload_json);
          return;
        }
        if (internal == "plan_reviewed")
        {
          handle_plan_reviewed (msg.payload_json);
          return;
        }
        if (internal == "forge_decision")
        {
          handle_forge_decision (msg.payload_json);
          return;
        }
      }
      handle_scheduled_task (msg);
      return;
    }

    switch (msg.kind)
    {
    case MasterEvent::Kind::JobSubmit:
      handle_job_submit (std::move (msg));
      break;
    case MasterEvent::Kind::WorkerExhausted:
      handle_worker_exhausted (std::move (msg));
      break;
    case MasterEvent::Kind::AdviserFailed:
      handle_adviser_failed (std::move (msg));
      break;
    case MasterEvent::Kind::ScheduledTask:
      handle_scheduled_task (std::move (msg));
      break;
    }
  }

  // ---------------------------------------------------------------------------
  // handle_job_submit — detach LLM thread for adviser selection
  // ---------------------------------------------------------------------------

  void Master::handle_job_submit (MasterEvent msg)
  {
    const std::string job_id = msg.job_id;

    // Parse goal from payload.
    rapidjson::Document doc;
    std::string goal;
    if (!doc.Parse (msg.payload_json.c_str ()).HasParseError ()
        && doc.HasMember ("goal") && doc["goal"].IsString ())
      goal = doc["goal"].GetString ();

    if (goal.empty ())
    {
      spdlog::error ("[master] job_submit: missing goal for job {}", job_id);
      OrchestratorEvent ev;
      ev.kind = OrchestratorEvent::Kind::MasterDecision;
      ev.job_id = job_id;
      ev.payload_json = R"({"type":"job_failed","job_id":")" + job_id
                        + R"(","reason":"missing goal"})";
      send_to_orchestrator_ (std::move (ev));
      return;
    }

    spdlog::info ("[master] job {} submitted, selecting adviser", job_id);

    // Detach LLM thread — result enqueued back to Master via ScheduledTask.
    std::thread (
      [this, job_id, goal] ()
      {
        const std::string adviser_id = select_adviser (job_id, goal);

        // Build internal result payload.
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w (buf);
        w.StartObject ();
        w.Key ("_internal");
        w.String ("adviser_selected");
        w.Key ("job_id");
        w.String (job_id.c_str ());
        w.Key ("adviser_id");
        w.String (adviser_id.c_str ());
        w.Key ("goal");
        w.String (goal.c_str ());
        w.EndObject ();

        MasterEvent result;
        result.kind = MasterEvent::Kind::ScheduledTask; // reused as internal
        result.job_id = job_id;
        result.payload_json = buf.GetString ();
        enqueue (std::move (result));
      })
      .detach ();
  }

  // ---------------------------------------------------------------------------
  // handle_adviser_selected — tell Orchestrator to spawn the Adviser
  // ---------------------------------------------------------------------------

  void Master::handle_adviser_selected (const std::string &payload_json)
  {
    rapidjson::Document doc;
    if (doc.Parse (payload_json.c_str ()).HasParseError ())
      return;

    const std::string job_id
      = doc.HasMember ("job_id") && doc["job_id"].IsString ()
          ? doc["job_id"].GetString ()
          : "";
    const std::string adviser_id
      = doc.HasMember ("adviser_id") && doc["adviser_id"].IsString ()
          ? doc["adviser_id"].GetString ()
          : "";
    const std::string goal = doc.HasMember ("goal") && doc["goal"].IsString ()
                               ? doc["goal"].GetString ()
                               : "";

    if (adviser_id.empty ())
    {
      spdlog::error ("[master] adviser selection failed for job {}", job_id);
      OrchestratorEvent ev;
      ev.kind = OrchestratorEvent::Kind::MasterDecision;
      ev.job_id = job_id;
      ev.payload_json = R"({"type":"job_failed","job_id":")" + job_id
                        + R"(","reason":"no adviser available"})";
      send_to_orchestrator_ (std::move (ev));
      return;
    }

    spdlog::info ("[master] job {} → adviser {}", job_id, adviser_id);

    // Tell Orchestrator to spawn this Adviser.
    OrchestratorEvent ev;
    ev.kind = OrchestratorEvent::Kind::MasterDecision;
    ev.job_id = job_id;
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("type");
    w.String ("spawn_adviser");
    w.Key ("job_id");
    w.String (job_id.c_str ());
    w.Key ("adviser_id");
    w.String (adviser_id.c_str ());
    w.Key ("goal");
    w.String (goal.c_str ());
    w.EndObject ();
    ev.payload_json = buf.GetString ();
    send_to_orchestrator_ (std::move (ev));
  }

  // ---------------------------------------------------------------------------
  // handle_plan_reviewed — tell Orchestrator to proceed or fail
  // ---------------------------------------------------------------------------

  void Master::handle_plan_reviewed (const std::string &payload_json)
  {
    rapidjson::Document doc;
    if (doc.Parse (payload_json.c_str ()).HasParseError ())
      return;

    const std::string job_id
      = doc.HasMember ("job_id") && doc["job_id"].IsString ()
          ? doc["job_id"].GetString ()
          : "";
    const bool approved
      = doc.HasMember ("approved") && doc["approved"].IsBool ()
          ? doc["approved"].GetBool ()
          : false;
    const std::string reason
      = doc.HasMember ("reason") && doc["reason"].IsString ()
          ? doc["reason"].GetString ()
          : "";
    const std::string plan_json
      = doc.HasMember ("plan_json") && doc["plan_json"].IsString ()
          ? doc["plan_json"].GetString ()
          : "";

    OrchestratorEvent ev;
    ev.kind = OrchestratorEvent::Kind::MasterDecision;
    ev.job_id = job_id;

    if (approved)
    {
      spdlog::info ("[master] plan approved for job {}", job_id);
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      w.StartObject ();
      w.Key ("type");
      w.String ("plan_ready");
      w.Key ("job_id");
      w.String (job_id.c_str ());
      w.Key ("plan_json");
      w.String (plan_json.c_str ());
      w.EndObject ();
      ev.payload_json = buf.GetString ();
    }
    else
    {
      spdlog::warn ("[master] plan rejected for job {}: {}", job_id, reason);
      ev.payload_json = R"({"type":"job_failed","job_id":")" + job_id
                        + R"(","reason":"plan rejected: )" + reason + R"("})";
    }

    send_to_orchestrator_ (std::move (ev));
  }

  // ---------------------------------------------------------------------------
  // handle_worker_exhausted — detach LLM thread to decide Forge
  // ---------------------------------------------------------------------------

  void Master::handle_worker_exhausted (MasterEvent msg)
  {
    const std::string job_id = msg.job_id;

    rapidjson::Document doc;
    std::string command, step_description;
    bool needs_forge = false;
    if (!doc.Parse (msg.payload_json.c_str ()).HasParseError ())
    {
      if (doc.HasMember ("command") && doc["command"].IsString ())
        command = doc["command"].GetString ();
      if (doc.HasMember ("needs_forge") && doc["needs_forge"].IsBool ())
        needs_forge = doc["needs_forge"].GetBool ();
      if (doc.HasMember ("step_description")
          && doc["step_description"].IsString ())
        step_description = doc["step_description"].GetString ();
    }

    spdlog::info ("[master] worker exhausted for job {} command={} "
                  "needs_forge={}",
                  job_id, command, needs_forge);

    // ADR-031: Planning Adviser declared needs_forge=true — skip decide_forge
    // LLM call and trigger Forge immediately with step description context.
    if (needs_forge)
    {
      spdlog::info ("[master] needs_forge=true, bypassing decide_forge for "
                    "job {} command={}",
                    job_id, command);
      OrchestratorEvent ev;
      ev.kind = OrchestratorEvent::Kind::MasterDecision;
      ev.job_id = job_id;
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      w.StartObject ();
      w.Key ("type");
      w.String ("trigger_forge");
      w.Key ("job_id");
      w.String (job_id.c_str ());
      w.Key ("command");
      w.String (command.c_str ());
      w.Key ("step_description");
      w.String (step_description.c_str ());
      w.EndObject ();
      ev.payload_json = buf.GetString ();
      send_to_orchestrator_ (std::move (ev));
      return;
    }

    std::thread (
      [this, job_id, command, step_description] ()
      {
        const bool trigger = decide_forge (job_id, command);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w (buf);
        w.StartObject ();
        w.Key ("_internal");
        w.String ("forge_decision");
        w.Key ("job_id");
        w.String (job_id.c_str ());
        w.Key ("trigger_forge");
        w.Bool (trigger);
        w.Key ("command");
        w.String (command.c_str ());
        w.Key ("step_description");
        w.String (step_description.c_str ());
        w.EndObject ();

        MasterEvent result;
        result.kind = MasterEvent::Kind::ScheduledTask;
        result.job_id = job_id;
        result.payload_json = buf.GetString ();
        enqueue (std::move (result));
      })
      .detach ();
  }

  // ---------------------------------------------------------------------------
  // handle_forge_decision — tell Orchestrator to trigger Forge or fail
  // ---------------------------------------------------------------------------

  void Master::handle_forge_decision (const std::string &payload_json)
  {
    rapidjson::Document doc;
    if (doc.Parse (payload_json.c_str ()).HasParseError ())
      return;

    const std::string job_id
      = doc.HasMember ("job_id") && doc["job_id"].IsString ()
          ? doc["job_id"].GetString ()
          : "";
    const bool trigger
      = doc.HasMember ("trigger_forge") && doc["trigger_forge"].IsBool ()
          ? doc["trigger_forge"].GetBool ()
          : false;
    const std::string command
      = doc.HasMember ("command") && doc["command"].IsString ()
          ? doc["command"].GetString ()
          : "";
    const std::string step_description
      = doc.HasMember ("step_description") && doc["step_description"].IsString ()
          ? doc["step_description"].GetString ()
          : "";

    OrchestratorEvent ev;
    ev.kind = OrchestratorEvent::Kind::MasterDecision;
    ev.job_id = job_id;

    if (trigger)
    {
      spdlog::info ("[master] triggering Forge for job {} command={}", job_id,
                    command);
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      w.StartObject ();
      w.Key ("type");
      w.String ("trigger_forge");
      w.Key ("job_id");
      w.String (job_id.c_str ());
      w.Key ("command");
      w.String (command.c_str ());
      w.Key ("step_description");
      w.String (step_description.c_str ());
      w.EndObject ();
      ev.payload_json = buf.GetString ();
    }
    else
    {
      spdlog::warn ("[master] no Forge for job {} — failing", job_id);
      ev.payload_json
        = R"({"type":"job_failed","job_id":")" + job_id
          + R"(","reason":"no worker available and Forge declined"})";
    }

    send_to_orchestrator_ (std::move (ev));
  }

  // ---------------------------------------------------------------------------
  // handle_adviser_failed
  // ---------------------------------------------------------------------------

  void Master::handle_adviser_failed (MasterEvent msg)
  {
    spdlog::warn ("[master] adviser failed for job {}", msg.job_id);
    // For now, fail the job. Future: retry with a different Adviser.
    OrchestratorEvent ev;
    ev.kind = OrchestratorEvent::Kind::MasterDecision;
    ev.job_id = msg.job_id;
    ev.payload_json = R"({"type":"job_failed","job_id":")" + msg.job_id
                      + R"(","reason":"adviser failed"})";
    send_to_orchestrator_ (std::move (ev));
  }

  // ---------------------------------------------------------------------------
  // handle_scheduled_task
  // ---------------------------------------------------------------------------

  void Master::handle_scheduled_task (MasterEvent msg)
  {
    spdlog::debug ("[master] scheduled task for job {}", msg.job_id);
    // Placeholder for future periodic review / follow-up logic.
    (void)msg;
  }

  // ---------------------------------------------------------------------------
  // LLM helpers
  // ---------------------------------------------------------------------------

  std::string Master::select_adviser (const std::string &job_id,
                                      const std::string &goal)
  {
    // Step 1: tokenize goal (lowercase alpha‑numeric tokens)
    std::vector<std::string> tokens;
    {
      std::string word;
      for (char c : goal)
        {
          if (std::isalnum (static_cast<unsigned char> (c)))
            word += static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
          else if (!word.empty ())
            {
              tokens.push_back (word);
              word.clear ();
            }
        }
      if (!word.empty ())
        tokens.push_back (word);
    }

    std::vector<RegisteredAdviser> candidates
      = registry_.find_advisers_by_domain (tokens);

    spdlog::info ("[master] job {} domain candidates: {}", job_id,
                  candidates.size ());

    if (candidates.empty ())
      {
        // Fallback to built-in planning — but only if it actually exists in
        // the Registry. ADR-033 assumes 'planning' is always seeded
        // (ADR-018 seed_if_absent), but that assumption doesn't hold in a
        // deployment/test with an empty Registry. Blindly returning the
        // literal "planning" here would make handle_adviser_selected treat
        // selection as successful (it only checks for an empty string) even
        // though no such adviser is registered, silently skipping the
        // job_failed path this exact scenario is supposed to take.
        const auto all = registry_.all_advisers ();
        const bool planning_exists
          = std::any_of (all.begin (), all.end (),
                          [] (const RegisteredAdviser &a)
                          { return a.id.value () == "planning"; });

        if (!planning_exists)
          {
            spdlog::error ("[master] job {} no domain match and no "
                           "'planning' adviser registered",
                           job_id);
            return "";
          }

        spdlog::info ("[master] job {} no domain match, selecting 'planning'", job_id);
        return "planning";
      }

    if (candidates.size () == 1)
      return candidates[0].id.value ();

    // Step 2 — bounded LLM disambiguation
    std::string llm_choice = llm_disambiguate_adviser (goal, candidates);
    if (!llm_choice.empty ())
      return llm_choice;

    // Step 3 — deterministic fallback (candidates already sorted)
    spdlog::warn ("[master] job {} LLM disambiguation failed, "
                   "fallback to {} (priority desc, id asc)",
                   job_id, candidates[0].id.value ());
    return candidates[0].id.value ();
  }

  std::string Master::review_plan (const std::string &job_id,
                                   const std::string &plan_json)
  {
    LlmRequest req;
    req.system_prompt
      = "You are the Master of an agent orchestration system reviewing a plan. "
        "Respond with JSON: {\"approved\":true} or "
        "{\"approved\":false,\"reason\":\"...\"}. "
        "Plan: "
        + plan_json;
    req.user_prompt = "Does this plan correctly address the task goal?";
    req.max_tokens = 512;

    auto result = llm_.complete (req);
    if (!result.ok)
    {
      spdlog::warn ("[master] LLM plan review failed for job {}, approving",
                    job_id);
      return ""; // fail open
    }

    // Strip markdown fence if LLM wrapped response in ```json...```
    std::string llm_content = result.value.content;
    if (llm_content.size () >= 3 && llm_content.substr (0, 3) == "```")
    {
      auto first_nl = llm_content.find ('\n');
      if (first_nl != std::string::npos)
        llm_content = llm_content.substr (first_nl + 1);
      if (llm_content.size () >= 3
          && llm_content.substr (llm_content.size () - 3) == "```")
        llm_content.erase (llm_content.size () - 3);
      while (!llm_content.empty ()
             && (llm_content.back () == '\n' || llm_content.back () == '\r'
                 || llm_content.back () == ' '))
        llm_content.pop_back ();
    }

    rapidjson::Document doc;
    if (doc.Parse (llm_content.c_str ()).HasParseError ()
        || !doc.IsObject () || !doc.HasMember ("approved")
        || !doc["approved"].IsBool ())
    {
      spdlog::warn ("[master] unexpected plan review response, approving");
      return "";
    }

    if (doc["approved"].GetBool ())
      return "";

    return (doc.HasMember ("reason") && doc["reason"].IsString ())
             ? doc["reason"].GetString ()
             : "plan rejected by Master";
  }

  bool Master::decide_forge (const std::string &job_id,
                             const std::string &command)
  {
    LlmRequest req;
    req.system_prompt = "You are the Master of an agent orchestration system. "
                        "A required worker capability is missing. "
                        "Decide whether to trigger the Forge pipeline to "
                        "generate a new worker. "
                        "Respond with JSON: {\"trigger_forge\":true} or "
                        "{\"trigger_forge\":false}.";
    req.user_prompt = "Missing capability: " + command;
    req.max_tokens = 128;

    auto result = llm_.complete (req);
    if (!result.ok)
    {
      spdlog::warn ("[master] LLM forge decision failed for job {}, triggering",
                    job_id);
      return true; // fail open — try Forge
    }

    // Strip markdown fence if LLM wrapped response in ```json...```
    std::string llm_content = result.value.content;
    if (llm_content.size () >= 3 && llm_content.substr (0, 3) == "```")
    {
      auto first_nl = llm_content.find ('\n');
      if (first_nl != std::string::npos)
        llm_content = llm_content.substr (first_nl + 1);
      if (llm_content.size () >= 3
          && llm_content.substr (llm_content.size () - 3) == "```")
        llm_content.erase (llm_content.size () - 3);
      while (!llm_content.empty ()
             && (llm_content.back () == '\n' || llm_content.back () == '\r'
                 || llm_content.back () == ' '))
        llm_content.pop_back ();
    }

    rapidjson::Document doc;
    if (doc.Parse (llm_content.c_str ()).HasParseError ()
        || !doc.IsObject () || !doc.HasMember ("trigger_forge")
        || !doc["trigger_forge"].IsBool ())
    {
      spdlog::warn ("[master] unexpected forge decision response, triggering");
      return true;
    }

    return doc["trigger_forge"].GetBool ();
  }

  std::string Master::build_adviser_context () const
  {
    const auto advisers = registry_.all_advisers ();
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartArray ();
    for (const auto &a : advisers)
    {
      w.StartObject ();
      w.Key ("id");
      w.String (a.id.value ().c_str ());
      w.Key ("name");
      w.String (a.name.c_str ());
      w.Key ("domains");
      w.StartArray ();
      for (const auto &d : a.domains)
        w.String (d.c_str ());
      w.EndArray ();
      w.EndObject ();
    }
    w.EndArray ();
    return buf.GetString ();
  }

  // ADR-033: bounded LLM disambiguation (Step 2) — returns empty on failure
  std::string Master::llm_disambiguate_adviser (
      const std::string &goal, const std::vector<RegisteredAdviser> &candidates) const
  {
    // Build user message as per ADR‑033
    std::string user_msg = "Goal: " + goal + "\n\nCandidates:\n";
    for (const auto &c : candidates)
    {
      user_msg += "- " + c.id.value () + ": ";
      if (!c.name.empty ())
        user_msg += c.name;
      if (!c.domains.empty ())
        {
          user_msg += " (domains: ";
          for (size_t i = 0; i < c.domains.size (); ++i)
            {
              if (i)
                user_msg += ", ";
              user_msg += c.domains[i];
            }
          user_msg += ")";
        }
      user_msg += "\n";
    }

    LlmRequest req;
    req.system_prompt
      = "You select exactly one adviser id from a candidate list.\n"
        "Respond with only a JSON object: {\"selected\": \"<adviser_id>\"}.\n"
        "No explanation, no markdown, no other text.";
    req.user_prompt = user_msg;
    req.max_tokens = 30;

    auto result = llm_fn_ ? llm_fn_(req) : llm_.complete (req);
    if (!result.ok)
      return {};

    // Strip markdown fence if present
    std::string content = result.value.content;
    if (content.size () >= 3 && content.substr (0, 3) == "```")
      {
        auto first_nl = content.find ('\n');
        if (first_nl != std::string::npos)
          content = content.substr (first_nl + 1);
        if (content.size () >= 3
            && content.substr (content.size () - 3) == "```")
          content.erase (content.size () - 3);
        while (!content.empty ()
               && (content.back () == '\n' || content.back () == '\r'
                   || content.back () == ' '))
          content.pop_back ();
      }

    rapidjson::Document doc;
    if (doc.Parse (content.c_str ()).HasParseError ()
        || !doc.IsObject ()
        || !doc.HasMember ("selected")
        || !doc["selected"].IsString ())
      return {};

    const std::string sel = doc["selected"].GetString ();
    for (const auto &c : candidates)
      if (c.id.value () == sel)
        return sel;

    // LLM returned an adviser_id not in the candidate set → fallback
    spdlog::warn ("[master] LLM disambiguation returned '{}' which is not "
                  "in the candidate set — ignoring",
                  sel);
    return {};
  }

} // namespace agentos
