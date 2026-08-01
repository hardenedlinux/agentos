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

    // Parse goal (and, if present, a known_adviser_id already resolved by
    // Orchestrator via explicit adviser_id or continuation lookup — see
    // cmd_job_submit) from payload.
    rapidjson::Document doc;
    std::string goal;
    std::string known_adviser_id;
    if (!doc.Parse (msg.payload_json.c_str ()).HasParseError ())
    {
      if (doc.HasMember ("goal") && doc["goal"].IsString ())
        goal = doc["goal"].GetString ();
      if (doc.HasMember ("known_adviser_id")
          && doc["known_adviser_id"].IsString ())
        known_adviser_id = doc["known_adviser_id"].GetString ();
    }

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

    spdlog::info ("[master] job {} submitted, running Digest Pass", job_id);

    // ADR-012 (amended): the Digest Pass always runs — exactly one
    // detached-thread LLM call per job, regardless of whether
    // known_adviser_id is already resolved. This is deliberate: Master's
    // own skill/prompt is expected to be iterated on repeatedly, and every
    // job must exercise the same code path so prompt changes are validated
    // against the full traffic mix, not just whatever happens not to
    // short-circuit around it. select_adviser() below owns Steps 0-3
    // (ADR-033) AND the mandatory Digest Pass call (ADR-012, folding in
    // the former Step 2 disambiguation per Amendment Note 3) as a single
    // unit — there is exactly one thread, one LLM round-trip, regardless
    // of which branch is taken internally.
    std::thread (
      [this, job_id, goal, known_adviser_id] ()
      {
        SelectionResult sel = select_adviser (job_id, goal, known_adviser_id);

        // Build internal result payload.
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w (buf);
        w.StartObject ();
        w.Key ("_internal");
        w.String ("adviser_selected");
        w.Key ("job_id");
        w.String (job_id.c_str ());
        w.Key ("adviser_id");
        w.String (sel.adviser_id.c_str ());
        w.Key ("goal");
        w.String (goal.c_str ());
        w.Key ("digested_problem");
        w.String (sel.digest.digested_problem.c_str ());
        w.Key ("deliverable_kind");
        w.String (sel.digest.deliverable_kind.c_str ());
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
    // ADR-012: Digest Pass output, always present on this internal event
    // now (handle_job_submit builds it unconditionally). Defaults mirror
    // DigestResult's own defaults in case of any malformed payload.
    const std::string digested_problem
      = doc.HasMember ("digested_problem") && doc["digested_problem"].IsString ()
          ? doc["digested_problem"].GetString ()
          : goal;
    const std::string deliverable_kind
      = doc.HasMember ("deliverable_kind") && doc["deliverable_kind"].IsString ()
          ? doc["deliverable_kind"].GetString ()
          : "result";

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

    spdlog::info ("[master] job {} → adviser {} (deliverable_kind={})",
                 job_id, adviser_id, deliverable_kind);

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
    w.Key ("digested_problem");
    w.String (digested_problem.c_str ());
    w.Key ("deliverable_kind");
    w.String (deliverable_kind.c_str ());
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

  Master::SelectionResult
  Master::select_adviser (const std::string &job_id, const std::string &goal,
                          const std::string &known_adviser_id)
  {
    // Step 0 — known_adviser_id short-circuit (ADR-033 §1 Step 0).
    // Still validated against the Registry first: a continuation row (or
    // an explicit job.submit adviser_id) can outlive its adviser
    // (revoked/removed) — an unvalidated forward would hand Orchestrator a
    // dead id instead of falling back to normal selection the way a
    // stale/mismatched continuation already degrades gracefully elsewhere.
    if (!known_adviser_id.empty ()
        && registry_.find_adviser_by_id (known_adviser_id))
    {
      spdlog::info ("[master] job {} routed via known adviser/continuation "
                    "owner '{}', skipping domain selection",
                    job_id, known_adviser_id);
      // ADR-012 Amendment: Digest Pass still runs — no candidates, since
      // selection is already decided; its adviser_id_suggestion (if any)
      // must not be read here.
      DigestResult digest = run_digest_pass (job_id, goal, {});
      return SelectionResult{known_adviser_id, std::move (digest)};
    }
    if (!known_adviser_id.empty ())
      spdlog::warn ("[master] job {} carried known_adviser_id '{}' but it "
                   "is no longer registered — falling back to normal "
                   "domain selection",
                   job_id, known_adviser_id);

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
        // deployment/test with an empty Registry.
        const auto all = registry_.all_advisers ();
        const bool planning_exists
          = std::any_of (all.begin (), all.end (),
                          [] (const RegisteredAdviser &a)
                          { return a.id.value () == "planning"; });

        // ADR-012 Amendment: Digest Pass still runs even on this
        // deterministic-fallback/failure path — no candidates to
        // disambiguate, but digestion/deliverable_kind must exist for
        // every job regardless of outcome.
        DigestResult digest = run_digest_pass (job_id, goal, {});

        if (!planning_exists)
          {
            spdlog::error ("[master] job {} no domain match and no "
                           "'planning' adviser registered",
                           job_id);
            return SelectionResult{"", std::move (digest)};
          }

        spdlog::info ("[master] job {} no domain match, selecting 'planning'", job_id);
        return SelectionResult{"planning", std::move (digest)};
      }

    if (candidates.size () == 1)
    {
      DigestResult digest = run_digest_pass (job_id, goal, {});
      return SelectionResult{candidates[0].id.value (), std::move (digest)};
    }

    // Step 2 (folded into the mandatory Digest Pass, ADR-033 Amendment
    // Note 3) — bounded LLM disambiguation, one output field of the same
    // call that also produces digested_problem/deliverable_kind.
    DigestResult digest = run_digest_pass (job_id, goal, candidates);

    if (!digest.adviser_id_suggestion.empty ())
    {
      for (const auto &c : candidates)
        if (c.id.value () == digest.adviser_id_suggestion)
          return SelectionResult{digest.adviser_id_suggestion,
                                 std::move (digest)};
      spdlog::warn ("[master] Digest Pass adviser_id_suggestion '{}' is "
                    "not in the candidate set — ignoring",
                    digest.adviser_id_suggestion);
    }

    // Step 3 — deterministic fallback (candidates already sorted)
    spdlog::warn ("[master] job {} disambiguation unresolved, "
                   "fallback to {} (priority desc, id asc)",
                   job_id, candidates[0].id.value ());
    return SelectionResult{candidates[0].id.value (), std::move (digest)};
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

  // ADR-012 (amended) + ADR-033 Amendment Note 3: the mandatory Digest
  // Pass. Always issues exactly one LLM call. `candidates` non-empty only
  // when the caller wants adviser_id_suggestion to be meaningful (Step 1
  // yielded >1 candidates and Step 0 did not resolve); empty otherwise —
  // the response's adviser_id_suggestion field is still populated by the
  // model but the caller must not consult it in that case.
  DigestResult Master::run_digest_pass (
      const std::string &job_id, const std::string &goal,
      const std::vector<RegisteredAdviser> &candidates) const
  {
    std::string user_msg = "Goal: " + goal;
    if (!candidates.empty ())
    {
      user_msg += "\n\nCandidates:\n";
      for (const auto &c : candidates)
      {
        user_msg += "- " + c.id.value () + ": ";
        // Prefer the manifest's natural-language description — this is
        // the actual signal ADR-033 intends for disambiguation. `name` is
        // just meta.id repeated (see parse_adviser_manifest_toml), so it
        // adds nothing beyond what the classifier already has in the id
        // itself.
        if (!c.description.empty ())
          user_msg += c.description;
        else if (!c.name.empty ())
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
    }

    LlmRequest req;
    req.system_prompt
      = "You are Master's Digest Pass for an agent orchestration system. "
        "For every job you must: "
        "1) produce a concise restatement of the user's goal that a "
        "downstream agent can act on directly (\"digested_problem\"); "
        "2) classify \"deliverable_kind\" as exactly \"artifact\" (the "
        "user wants a generated artifact itself — e.g. source code, a "
        "document, an image — not the result of running it against real "
        "input) or \"result\" (the user wants the output of running some "
        "capability against real input); "
        "3) if a Candidates list is provided below, select exactly one "
        "adviser id from it as \"adviser_id_suggestion\"; if no Candidates "
        "list is provided, set \"adviser_id_suggestion\" to an empty "
        "string.\n"
        "Respond with only a JSON object: {\"digested_problem\": \"...\", "
        "\"deliverable_kind\": \"artifact\"|\"result\", "
        "\"adviser_id_suggestion\": \"...\"}. "
        "No explanation, no markdown, no other text.";
    req.user_prompt = user_msg;
    req.max_tokens = 300;

    DigestResult digest; // deliverable_kind defaults to "result" (fail-open)

    auto result = llm_fn_ ? llm_fn_ (req) : llm_.complete (req);
    if (!result.ok)
    {
      spdlog::warn ("[master] Digest Pass LLM call failed for job {}, "
                    "using raw goal and default classification",
                    job_id);
      digest.digested_problem = goal;
      return digest;
    }

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
    if (doc.Parse (content.c_str ()).HasParseError () || !doc.IsObject ())
    {
      spdlog::warn ("[master] Digest Pass returned unparseable response "
                    "for job {}, using raw goal and default classification",
                    job_id);
      digest.digested_problem = goal;
      return digest;
    }

    digest.digested_problem
      = (doc.HasMember ("digested_problem") && doc["digested_problem"].IsString ())
          ? doc["digested_problem"].GetString ()
          : goal;

    if (doc.HasMember ("deliverable_kind") && doc["deliverable_kind"].IsString ())
    {
      const std::string dk = doc["deliverable_kind"].GetString ();
      if (dk == "artifact" || dk == "result")
        digest.deliverable_kind = dk;
      else
        spdlog::warn ("[master] Digest Pass returned unrecognized "
                      "deliverable_kind '{}' for job {}, defaulting to "
                      "'result'",
                      dk, job_id);
    }

    if (doc.HasMember ("adviser_id_suggestion")
        && doc["adviser_id_suggestion"].IsString ())
      digest.adviser_id_suggestion = doc["adviser_id_suggestion"].GetString ();

    return digest;
  }

} // namespace agentos
