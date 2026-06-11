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

#include <thread>

namespace agentos
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Master::Master (LlmClient          &llm,
                Registry           &registry,
                SendToOrchestrator  send_to_orchestrator)
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
            { handle_adviser_selected (msg.payload_json); return; }
          if (internal == "plan_reviewed")
            { handle_plan_reviewed (msg.payload_json); return; }
          if (internal == "forge_decision")
            { handle_forge_decision (msg.payload_json); return; }
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
      ev.kind         = OrchestratorEvent::Kind::MasterDecision;
      ev.job_id       = job_id;
      ev.payload_json = R"({"type":"job_failed","job_id":")" + job_id
                        + R"(","reason":"missing goal"})";
      send_to_orchestrator_ (std::move (ev));
      return;
    }

  spdlog::info ("[master] job {} submitted, selecting adviser", job_id);

  // Detach LLM thread — result enqueued back to Master via ScheduledTask.
  std::thread ([this, job_id, goal] ()
  {
    const std::string adviser_id = select_adviser (job_id, goal);

    // Build internal result payload.
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("_internal");   w.String ("adviser_selected");
    w.Key ("job_id");      w.String (job_id.c_str ());
    w.Key ("adviser_id");  w.String (adviser_id.c_str ());
    w.Key ("goal");        w.String (goal.c_str ());
    w.EndObject ();

    MasterEvent result;
    result.kind         = MasterEvent::Kind::ScheduledTask; // reused as internal
    result.job_id       = job_id;
    result.payload_json = buf.GetString ();
    enqueue (std::move (result));
  }).detach ();
}

// ---------------------------------------------------------------------------
// handle_adviser_selected — tell Orchestrator to spawn the Adviser
// ---------------------------------------------------------------------------

void Master::handle_adviser_selected (const std::string &payload_json)
{
  rapidjson::Document doc;
  if (doc.Parse (payload_json.c_str ()).HasParseError ())
    return;

  const std::string job_id =
    doc.HasMember ("job_id") && doc["job_id"].IsString ()
      ? doc["job_id"].GetString () : "";
  const std::string adviser_id =
    doc.HasMember ("adviser_id") && doc["adviser_id"].IsString ()
      ? doc["adviser_id"].GetString () : "";
  const std::string goal =
    doc.HasMember ("goal") && doc["goal"].IsString ()
      ? doc["goal"].GetString () : "";

  if (adviser_id.empty ())
    {
      spdlog::error ("[master] adviser selection failed for job {}", job_id);
      OrchestratorEvent ev;
      ev.kind         = OrchestratorEvent::Kind::MasterDecision;
      ev.job_id       = job_id;
      ev.payload_json = R"({"type":"job_failed","job_id":")" + job_id
                        + R"(","reason":"no adviser available"})";
      send_to_orchestrator_ (std::move (ev));
      return;
    }

  spdlog::info ("[master] job {} → adviser {}", job_id, adviser_id);

  // Tell Orchestrator to spawn this Adviser.
  OrchestratorEvent ev;
  ev.kind   = OrchestratorEvent::Kind::MasterDecision;
  ev.job_id = job_id;
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w (buf);
  w.StartObject ();
  w.Key ("type");        w.String ("spawn_adviser");
  w.Key ("job_id");      w.String (job_id.c_str ());
  w.Key ("adviser_id");  w.String (adviser_id.c_str ());
  w.Key ("goal");        w.String (goal.c_str ());
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

  const std::string job_id =
    doc.HasMember ("job_id") && doc["job_id"].IsString ()
      ? doc["job_id"].GetString () : "";
  const bool approved =
    doc.HasMember ("approved") && doc["approved"].IsBool ()
      ? doc["approved"].GetBool () : false;
  const std::string reason =
    doc.HasMember ("reason") && doc["reason"].IsString ()
      ? doc["reason"].GetString () : "";
  const std::string plan_json =
    doc.HasMember ("plan_json") && doc["plan_json"].IsString ()
      ? doc["plan_json"].GetString () : "";

  OrchestratorEvent ev;
  ev.kind   = OrchestratorEvent::Kind::MasterDecision;
  ev.job_id = job_id;

  if (approved)
    {
      spdlog::info ("[master] plan approved for job {}", job_id);
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      w.StartObject ();
      w.Key ("type");      w.String ("plan_ready");
      w.Key ("job_id");    w.String (job_id.c_str ());
      w.Key ("plan_json"); w.String (plan_json.c_str ());
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
  std::string command;
  if (!doc.Parse (msg.payload_json.c_str ()).HasParseError ()
      && doc.HasMember ("command") && doc["command"].IsString ())
    command = doc["command"].GetString ();

  spdlog::info ("[master] worker exhausted for job {} command={}",
                job_id, command);

  std::thread ([this, job_id, command] ()
  {
    const bool trigger = decide_forge (job_id, command);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("_internal");     w.String ("forge_decision");
    w.Key ("job_id");        w.String (job_id.c_str ());
    w.Key ("trigger_forge"); w.Bool (trigger);
    w.Key ("command");       w.String (command.c_str ());
    w.EndObject ();

    MasterEvent result;
    result.kind         = MasterEvent::Kind::ScheduledTask;
    result.job_id       = job_id;
    result.payload_json = buf.GetString ();
    enqueue (std::move (result));
  }).detach ();
}

// ---------------------------------------------------------------------------
// handle_forge_decision — tell Orchestrator to trigger Forge or fail
// ---------------------------------------------------------------------------

void Master::handle_forge_decision (const std::string &payload_json)
{
  rapidjson::Document doc;
  if (doc.Parse (payload_json.c_str ()).HasParseError ())
    return;

  const std::string job_id =
    doc.HasMember ("job_id") && doc["job_id"].IsString ()
      ? doc["job_id"].GetString () : "";
  const bool trigger =
    doc.HasMember ("trigger_forge") && doc["trigger_forge"].IsBool ()
      ? doc["trigger_forge"].GetBool () : false;
  const std::string command =
    doc.HasMember ("command") && doc["command"].IsString ()
      ? doc["command"].GetString () : "";

  OrchestratorEvent ev;
  ev.kind   = OrchestratorEvent::Kind::MasterDecision;
  ev.job_id = job_id;

  if (trigger)
    {
      spdlog::info ("[master] triggering Forge for job {} command={}",
                    job_id, command);
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      w.StartObject ();
      w.Key ("type");    w.String ("trigger_forge");
      w.Key ("job_id");  w.String (job_id.c_str ());
      w.Key ("command"); w.String (command.c_str ());
      w.EndObject ();
      ev.payload_json = buf.GetString ();
    }
  else
    {
      spdlog::warn ("[master] no Forge for job {} — failing", job_id);
      ev.payload_json = R"({"type":"job_failed","job_id":")" + job_id
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
  ev.kind         = OrchestratorEvent::Kind::MasterDecision;
  ev.job_id       = msg.job_id;
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
  const auto advisers = registry_.all_advisers ();
  if (advisers.empty ())
    {
      spdlog::error ("[master] no advisers registered");
      return "";
    }
  if (advisers.size () == 1)
    return advisers.front ().id.value ();

  LlmRequest req;
  req.system_prompt =
    "You are the Master of an agent orchestration system. "
    "Given a task goal and a list of available advisers, select the most "
    "appropriate adviser. "
    "Respond with a JSON object containing only 'adviser_id'. "
    "Available advisers: " + build_adviser_context ();
  req.user_prompt = goal;
  req.max_tokens  = 256;

  auto result = llm_.complete (req);
  if (!result.ok)
    {
      spdlog::error ("[master] LLM adviser selection failed for job {}: {}",
                     job_id, result.error);
      return advisers.front ().id.value (); // fallback to first
    }

  rapidjson::Document doc;
  if (doc.Parse (result.value.content.c_str ()).HasParseError ()
      || !doc.IsObject () || !doc.HasMember ("adviser_id")
      || !doc["adviser_id"].IsString ())
    {
      spdlog::warn ("[master] unexpected adviser selection response, using first");
      return advisers.front ().id.value ();
    }

  return doc["adviser_id"].GetString ();
}

std::string Master::review_plan (const std::string &job_id,
                                 const std::string &plan_json)
{
  LlmRequest req;
  req.system_prompt =
    "You are the Master of an agent orchestration system reviewing a plan. "
    "Respond with JSON: {\"approved\":true} or "
    "{\"approved\":false,\"reason\":\"...\"}. "
    "Plan: " + plan_json;
  req.user_prompt = "Does this plan correctly address the task goal?";
  req.max_tokens  = 512;

  auto result = llm_.complete (req);
  if (!result.ok)
    {
      spdlog::warn ("[master] LLM plan review failed for job {}, approving",
                    job_id);
      return ""; // fail open
    }

  rapidjson::Document doc;
  if (doc.Parse (result.value.content.c_str ()).HasParseError ()
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
  req.system_prompt =
    "You are the Master of an agent orchestration system. "
    "A required worker capability is missing. "
    "Decide whether to trigger the Forge pipeline to generate a new worker. "
    "Respond with JSON: {\"trigger_forge\":true} or {\"trigger_forge\":false}.";
  req.user_prompt = "Missing capability: " + command;
  req.max_tokens  = 128;

  auto result = llm_.complete (req);
  if (!result.ok)
    {
      spdlog::warn ("[master] LLM forge decision failed for job {}, triggering",
                    job_id);
      return true; // fail open — try Forge
    }

  rapidjson::Document doc;
  if (doc.Parse (result.value.content.c_str ()).HasParseError ()
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
      w.Key ("id");   w.String (a.id.value ().c_str ());
      w.Key ("name"); w.String (a.name.c_str ());
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

} // namespace agentos
