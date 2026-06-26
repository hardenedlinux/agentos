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
 * agentos/orchestrator.cpp
 *
 * ADR-022: Orchestrator — authentication, pipeline execution, coordination.
 * ADR-005: Persist before act.
 * ADR-009: No LLM calls; all logic is deterministic.
 */

#include "agentos/orchestrator.h"
#include "agentos/cred_vault.h"
#include "agentos/home_init.h"
#include "agentos/time_utils.h"
#include "agentos/user_manager.h"
#include "agentos/uuid.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>

#include <fstream>
#include <iostream>
#include <thread>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <unordered_set>

namespace agentos
{

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------

  namespace
  {
    // Constant-time comparison to prevent timing attacks on key verification.
    bool ct_equal (std::string_view a, std::string_view b)
    {
      if (a.size () != b.size ())
        return false;
      volatile int diff = 0;
      for (size_t i = 0; i < a.size (); ++i)
        diff |= (static_cast<unsigned char> (a[i])
                 ^ static_cast<unsigned char> (b[i]));
      return diff == 0;
    }

    // SHA-256(key || salt) → hex string (ADR-020).
    std::string sha256_hex (const std::string &key, const std::string &salt)
    {
      const std::string data = key + salt;
      unsigned char hash[SHA256_DIGEST_LENGTH];
      EVP_MD_CTX *ctx = EVP_MD_CTX_new ();
      EVP_DigestInit_ex (ctx, EVP_sha256 (), nullptr);
      EVP_DigestUpdate (ctx, data.data (), data.size ());
      EVP_DigestFinal_ex (ctx, hash, nullptr);
      EVP_MD_CTX_free (ctx);

      char hex[SHA256_DIGEST_LENGTH * 2 + 1];
      for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        snprintf (hex + i * 2, 3, "%02x", hash[i]);
      return std::string (hex, SHA256_DIGEST_LENGTH * 2);
    }

    // Build a JSON-RPC 2.0 response envelope.
    std::string make_response (const std::string &id,
                               const std::string &result_json)
    {
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      w.StartObject ();
      w.Key ("jsonrpc");
      w.String ("2.0");
      w.Key ("id");
      w.String (id.c_str ());
      w.Key ("result");
      w.RawValue (result_json.c_str (), result_json.size (),
                  rapidjson::kObjectType);
      w.EndObject ();
      return buf.GetString ();
    }

    std::string make_error_response (const std::string &id, int code,
                                     const std::string &message)
    {
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      w.StartObject ();
      w.Key ("jsonrpc");
      w.String ("2.0");
      w.Key ("id");
      w.String (id.c_str ());
      w.Key ("error");
      w.StartObject ();
      w.Key ("code");
      w.Int (code);
      w.Key ("message");
      w.String (message.c_str ());
      w.EndObject ();
      w.EndObject ();
      return buf.GetString ();
    }

    std::string make_notification (const std::string &method,
                                   const std::string &params_json)
    {
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      w.StartObject ();
      w.Key ("jsonrpc");
      w.String ("2.0");
      w.Key ("method");
      w.String (method.c_str ());
      w.Key ("params");
      w.RawValue (params_json.c_str (), params_json.size (),
                  rapidjson::kObjectType);
      w.EndObject ();
      return buf.GetString ();
    }

    // Role permission matrix (ADR-025).
    bool role_permitted (const std::string &role, const std::string &method)
    {
      if (role == "admin")
        return true; // admin can do everything
      if (role == "operator")
      {
        static const std::unordered_set<std::string> operator_methods = {
          "job.submit",
          "job.status",
          "job.list",
          "job.cancel",
          "worker.list",
          "adviser.list",
          "review.list",
          "review.show",
          "review.approve",
          "review.reject",
          "forge.list",
          "forge.status",
          // ADR-028: operators can manage credentials
          "cred.submit",
          "cred.revoke",
          "cred.list",
          "cred.audit",
        };
        return operator_methods.count (method) > 0;
      }
      if (role == "readonly")
      {
        static const std::unordered_set<std::string> readonly_methods = {
          "job.status",  "job.list",    "worker.list", "adviser.list",
          "review.list", "review.show", "forge.list",  "forge.status",
        };
        return readonly_methods.count (method) > 0;
      }
      return false;
    }

  } // anonymous namespace

  // ---------------------------------------------------------------------------
  // Construction
  // ---------------------------------------------------------------------------

  Orchestrator::Orchestrator (Database &db, LlmProxy &llm, Registry &registry,
                              Dispatcher &dispatcher,
                              forge::ForgeCoordinator &forge,
                              const Config &config, CredVault &cred_vault,
                              SendToMaster send_to_master,
                              SendToGateway send_to_gateway)
    : db_ (db), llm_ (llm), registry_ (registry), dispatcher_ (dispatcher),
      forge_ (forge), config_ (config), cred_vault_ (cred_vault),
      send_to_master_ (std::move (send_to_master)),
      send_to_gateway_ (std::move (send_to_gateway)), user_manager_ (db)
  {
  }

  // ---------------------------------------------------------------------------
  // init — called before start()
  // ---------------------------------------------------------------------------

  void Orchestrator::init ()
  {
    // Load access keys cache (ADR-022).
    load_active_keys ();

    // Crash recovery (ADR-005, ADR-016):
    // Mark any worker_runs with status='running' as 'crashed'.
    db_.mark_all_running_as_crashed ();

    // Reconstruct in-memory job index from DB.
    auto in_flight = db_.resume_in_flight ();
    for (const auto &rec : in_flight)
    {
      // Reconstruct ActiveJob from DB record.
      // Steps are re-loaded and pending ones re-queued.
      ActiveJob job;
      job.job_id = rec.job_id.value ();
      job.type = "oneshot"; // TODO: persist type in jobs table
      // For now push a placeholder — real step reconstruction
      // requires loading tasks table by job_id (future ADR-022 impl).
      active_jobs_[job.job_id] = std::move (job);
      spdlog::info ("[orchestrator] recovered in-flight job {}", job.job_id);
    }

    // Register Dispatcher reap callback — fires on the PeriodicExecutor thread.
    // Enqueues OrchestratorEvent which is consumed on the Orchestrator thread.
    dispatcher_.set_reap_callback (
      [this] (WorkerExited ev)
      {
        const int exit_code = ev.exit_code;
        if (exit_code == 0)
        {
          OrchestratorEvent oe;
          oe.kind = OrchestratorEvent::Kind::WorkerDone;
          oe.job_id = ev.run_id; // Orchestrator resolves job via run_id
          oe.payload_json = R"({"run_id":")" + ev.run_id + R"(","step_id":")"
                            + ev.step_id + R"(","exit_code":0,"job_dir":")"
                            + ev.job_dir + R"("})";
          enqueue (std::move (oe));
        }
        else
        {
          OrchestratorEvent oe;
          oe.kind = OrchestratorEvent::Kind::WorkerFailed;
          oe.job_id = ev.run_id;
          oe.payload_json = R"({"run_id":")" + ev.run_id + R"(","step_id":")"
                            + ev.step_id + R"(","exit_code":)"
                            + std::to_string (exit_code) + R"(,"job_dir":")"
                            + ev.job_dir + R"("})";
          enqueue (std::move (oe));
        }
      });

    // Register ForgeCoordinator completion callback.
    forge_.set_completion_callback (
      [this] (forge::ForgeResult result)
      {
        OrchestratorEvent oe;
        oe.kind = OrchestratorEvent::Kind::MasterDecision;
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w (buf);
        w.StartObject ();
        w.Key ("type");
        w.String ("forge_complete");
        w.Key ("forge_job_id");
        w.String (result.forge_job_id.c_str ());
        w.Key ("task_id");
        w.String (result.task_id.c_str ());
        w.Key ("outcome");
        w.Int (static_cast<int> (result.outcome));
        w.Key ("worker_id");
        w.String (result.worker_id.c_str ());
        w.Key ("review_id");
        w.String (result.review_id.c_str ());
        w.Key ("error");
        w.String (result.error.c_str ());
        w.EndObject ();
        oe.payload_json = buf.GetString ();
        enqueue (std::move (oe));
      });

    spdlog::info ("[orchestrator] initialised");

    // ADR-029: seed default user_id="0" for single-user CLI deployments.
    if (auto res = user_manager_.register_user ("0"); !res)
      spdlog::warn ("[orchestrator] failed to seed default user: {}",
                    res.error ());
  }

  // ---------------------------------------------------------------------------
  // Actor: on_message — serial dispatch
  // ---------------------------------------------------------------------------

  void Orchestrator::on_message (OrchestratorEvent msg)
  {
    switch (msg.kind)
    {
    case OrchestratorEvent::Kind::GatewayInbound:
      handle_gateway_inbound (msg);
      break;
    case OrchestratorEvent::Kind::WorkerDone:
      handle_worker_done (msg);
      break;
    case OrchestratorEvent::Kind::WorkerFailed:
      handle_worker_failed (msg);
      break;
    case OrchestratorEvent::Kind::AdviserDone:
      handle_adviser_done (msg);
      break;
    case OrchestratorEvent::Kind::AdviserFailed:
      handle_adviser_failed (msg);
      break;
    case OrchestratorEvent::Kind::MasterDecision:
      handle_master_decision (msg);
      break;
    case OrchestratorEvent::Kind::TimerFired:
      handle_timer_fired (msg);
      break;
    }
  }

  // ---------------------------------------------------------------------------
  // Authentication
  // ---------------------------------------------------------------------------

  void Orchestrator::load_active_keys ()
  {
    active_keys_.clear ();
    auto keys = db_.load_active_access_keys ();
    for (auto &k : keys)
    {
      // spdlog::info ("load key: {}", k.key);
      active_keys_[k.key] = std::move (k);
    }
    spdlog::info ("[orchestrator] loaded {} active access keys",
                  active_keys_.size ());
  }

  std::optional<Database::AccessKey>
  Orchestrator::authenticate (const std::string &key_value) const
  {
    // Reject malformed keys before any processing.
    // Hex-encoded 32 bytes = exactly 64 chars, [0-9a-f] only.
    if (key_value.size () != 64)
      return std::nullopt;
    for (char c : key_value)
    {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        return std::nullopt;
    }

    auto it = active_keys_.find (key_value);
    if (it == active_keys_.end ())
      return std::nullopt;
    const auto &ak = it->second;
    const std::string computed = sha256_hex (key_value, ak.key_salt);
    if (!ct_equal (computed, ak.key_hash))
      return std::nullopt;
    return ak;
  }

  bool Orchestrator::is_permitted (const std::string &role,
                                   const std::string &method) const
  {
    return role_permitted (role, method);
  }

  // ---------------------------------------------------------------------------
  // GatewayInbound — parse JSON-RPC, authenticate, route
  // ---------------------------------------------------------------------------

  void Orchestrator::handle_gateway_inbound (const OrchestratorEvent &ev)
  {
    spdlog::info ("[orchestrator] inbound method");

    rapidjson::Document doc;
    if (doc.Parse (ev.payload_json.c_str ()).HasParseError ())
    {
      // Can't even extract identity/id — log and drop.
      spdlog::warn ("[orchestrator] parse error on inbound message");
      return;
    }

    // identity comes from the ZMQ identity frame, attached to the event by
    // Central's forward_fn. It is not present in the JSON payload — the
    // previous "_identity" lookup was a no-op that left every response
    // unroutable.
    const std::string identity = ev.identity;
    const std::string request_id = doc.HasMember ("id") && doc["id"].IsString ()
                                     ? doc["id"].GetString ()
                                     : "";
    const std::string method
      = doc.HasMember ("method") && doc["method"].IsString ()
          ? doc["method"].GetString ()
          : "";
    const std::string key_value
      = doc.HasMember ("key") && doc["key"].IsString ()
          ? doc["key"].GetString ()
          : "";

    // 1. Missing key.
    if (key_value.empty ())
    {
      reply_error (identity, request_id, -32010, "Missing key, unauthorized");
      return;
    }

    // 2. Authenticate.
    auto ak = authenticate (key_value);
    if (!ak)
    {
      reply_error (identity, request_id, -32010, "Failed to authorize");
      return;
    }

    // 3. Update last_used_at asynchronously (fire and forget — DB write,
    //    acceptable to do synchronously here since Orchestrator is the sole
    //    writer).
    db_.touch_access_key (ak->id);

    // 4. Permission check.
    if (!is_permitted (ak->role, method))
    {
      reply_error (identity, request_id, -32011, "Forbidden");
      return;
    }

    // 5. Extract params.
    std::string params_json = "{}";
    if (doc.HasMember ("params"))
    {
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      doc["params"].Accept (w);
      params_json = buf.GetString ();
    }

    // 6. Route to command handler.
    if (method == "job.submit")
      cmd_job_submit (params_json, identity, request_id);
    else if (method == "job.status")
      cmd_job_status (params_json, identity, request_id);
    else if (method == "job.list")
      cmd_job_list (params_json, identity, request_id);
    else if (method == "job.cancel")
      cmd_job_cancel (params_json, identity, request_id);
    else if (method == "review.approve")
      cmd_review_approve (params_json, identity, request_id);
    else if (method == "review.reject")
      cmd_review_reject (params_json, identity, request_id);
    else if (method == "worker.register")
      cmd_worker_register (params_json, identity, request_id);
    else if (method == "worker.list")
      cmd_worker_list (params_json, identity, request_id);
    else if (method == "adviser.list")
      cmd_adviser_list (params_json, identity, request_id);
    // --- ADR-028: credential vault methods ---
    else if (method == "cred.submit")
      cmd_cred_submit (params_json, identity, request_id);
    else if (method == "cred.revoke")
      cmd_cred_revoke (params_json, identity, request_id);
    else if (method == "cred.grant")
      cmd_cred_grant (params_json, identity, request_id);
    else if (method == "cred.revoke_grant")
      cmd_cred_revoke_grant (params_json, identity, request_id);
    else if (method == "cred.list")
      cmd_cred_list (params_json, identity, request_id);
    else if (method == "cred.audit")
      cmd_cred_audit (params_json, identity, request_id);
    else if (method == "vault.rekey")
      cmd_vault_rekey (params_json, identity, request_id);
    // --- ADR-029: user management methods (admin only) ---
    else if (method == "user.register")
      cmd_user_register (params_json, identity, request_id);
    else if (method == "user.list")
      cmd_user_list (params_json, identity, request_id);
    else if (method == "user.enable")
      cmd_user_enable (params_json, identity, request_id);
    else if (method == "user.disable")
      cmd_user_disable (params_json, identity, request_id);
    else if (method == "user.profile")
      cmd_user_profile (params_json, identity, request_id);
    else
      reply_error (identity, request_id, -32601, "Method not found");
  }

  // ---------------------------------------------------------------------------
  // Command: job.submit
  // ---------------------------------------------------------------------------

  void Orchestrator::cmd_job_submit (const std::string &params_json,
                                     const std::string &identity,
                                     const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("goal") || !params["goal"].IsString ())
    {
      reply_error (identity, request_id, -32602,
                   "Invalid params: missing goal");
      return;
    }

    // ADR-029: user_id defaults to "0" when absent (CLI single-user mode).
    // An explicitly empty string is an error.
    std::string user_id = "0";
    if (params.HasMember ("user_id") && params["user_id"].IsString ())
      user_id = params["user_id"].GetString ();
    if (user_id.empty ())
    {
      reply_error (identity, request_id, -32602,
                   "Invalid params: user_id must not be empty");
      return;
    }

    // Validate user — absent and disabled both return -32020 (ADR-029).
    if (auto v = user_manager_.validate_user (user_id); !v)
    {
      reply_error (identity, request_id, -32020, "Not found");
      return;
    }

    const std::string goal = params["goal"].GetString ();
    const std::string type
      = (params.HasMember ("type") && params["type"].IsString ())
          ? params["type"].GetString ()
          : "oneshot";
    const std::string job_id = new_uuid ();

    // Persist job (phase = planning).
    Task task;
    task.id = TaskId (job_id);
    task.goal = goal;
    task.input_json = (params.HasMember ("input")) ? params_json : "{}";
    task.user_id = user_id;
    db_.store_job (task);
    db_.update_job_phase (TaskId (job_id), "planning");
    db_.update_job_type (job_id, type);

    spdlog::info ("[orchestrator] job.submit job_id={} user_id={} goal='{}'",
                  job_id, user_id, goal);

    // Reply immediately — job runs asynchronously.
    reply_ok (identity, request_id, R"({"job_id":")" + job_id + R"("})");

    // Forward to Master for planning.
    MasterEvent me;
    me.kind = MasterEvent::Kind::JobSubmit;
    me.job_id = job_id;
    me.payload_json = R"({"job_id":")" + job_id + R"(","goal":")" + goal
                      + R"(","type":")" + type + R"("})";
    send_to_master_ (std::move (me));
  }

  // ---------------------------------------------------------------------------
  // Command: job.status
  // ---------------------------------------------------------------------------
  void Orchestrator::cmd_job_status (const std::string &params_json,
                                     const std::string &identity,
                                     const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("job_id") || !params["job_id"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    const std::string job_id = params["job_id"].GetString ();

    auto job = db_.load_job (job_id);
    if (!job)
    {
      reply_error (identity, request_id, -32020, "Not found");
      return;
    }

    auto steps = db_.load_steps_for_job (job_id);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("job_id");
    w.String (job_id.c_str ());
    w.Key ("phase");
    w.String (job->phase.c_str ());
    w.Key ("goal");
    w.String (job->goal.c_str ());
    w.Key ("created_at");
    w.Int64 (job->created_at);
    w.Key ("updated_at");
    w.Int64 (job->updated_at);
    if (job->error)
    {
      w.Key ("error");
      w.String (job->error->c_str ());
    }
    w.Key ("steps");
    w.StartArray ();
    for (const auto &s : steps)
    {
      w.StartObject ();
      w.Key ("id");
      w.String (s.id.c_str ());
      w.Key ("step_order");
      w.Int (s.step_order);
      w.Key ("description");
      w.String (s.description.c_str ());
      w.Key ("status");
      w.String (s.status.c_str ());
      w.Key ("started_at");
      if (s.started_at)
        w.Int64 (*s.started_at);
      else
        w.Null ();
      w.Key ("completed_at");
      if (s.completed_at)
        w.Int64 (*s.completed_at);
      else
        w.Null ();
      if (s.error)
      {
        w.Key ("error");
        w.String (s.error->c_str ());
      }
      w.EndObject ();
    }
    w.EndArray ();
    w.EndObject ();

    reply_ok (identity, request_id, buf.GetString ());
  }

  // ---------------------------------------------------------------------------
  // Command: job.list
  // ---------------------------------------------------------------------------
  void Orchestrator::cmd_job_list (const std::string &params_json,
                                   const std::string &identity,
                                   const std::string &request_id)
  {
    spdlog::info ("[orchestrator] cmd_job_list called");

    rapidjson::Document params;
    params.Parse (params_json.c_str ());

    std::optional<std::string> user_id_filter;
    if (!params.HasParseError ()
        && params.HasMember ("user_id") && params["user_id"].IsString ())
      user_id_filter = params["user_id"].GetString ();

    // --all: no time filter; --since <minutes>: override default 10min window
    std::optional<int64_t> since_unix = now_unix () - 600; // default: 10 min
    if (!params.HasParseError ())
    {
      if (params.HasMember ("all") && params["all"].IsBool ()
          && params["all"].GetBool ())
        since_unix = std::nullopt;
      else if (params.HasMember ("since_minutes") && params["since_minutes"].IsInt ())
        since_unix = now_unix () - static_cast<int64_t> (params["since_minutes"].GetInt ()) * 60;
    }

    int limit = 100;
    if (!params.HasParseError () && params.HasMember ("limit") && params["limit"].IsInt ())
      limit = params["limit"].GetInt ();

    auto jobs = db_.load_jobs_since (since_unix, limit, user_id_filter);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("jobs");
    w.StartArray ();
    for (const auto &j : jobs)
    {
      w.StartObject ();
      w.Key ("id");
      w.String (j.id.c_str ());
      w.Key ("type");
      w.String (j.type.c_str ());
      w.Key ("phase");
      w.String (j.phase.c_str ());
      w.Key ("goal");
      w.String (j.goal.c_str ());
      w.Key ("user_id");
      w.String (j.user_id.c_str ());
      w.Key ("created_at");
      w.Int64 (j.created_at);
      w.Key ("updated_at");
      w.Int64 (j.updated_at);
      if (j.error)
      {
        w.Key ("error");
        w.String (j.error->c_str ());
      }
      w.EndObject ();
    }
    w.EndArray ();
    w.Key ("total");
    w.Int (static_cast<int> (jobs.size ()));
    w.EndObject ();

    reply_ok (identity, request_id, buf.GetString ());
  }

  // ---------------------------------------------------------------------------
  // Command: job.cancel
  // ---------------------------------------------------------------------------

  void Orchestrator::cmd_job_cancel (const std::string &params_json,
                                     const std::string &identity,
                                     const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("job_id") || !params["job_id"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    const std::string job_id = params["job_id"].GetString ();

    // Job actively executing — remove from active_jobs_.
    auto it = active_jobs_.find (job_id);
    if (it != active_jobs_.end ())
    {
      active_jobs_.erase (it);
    }
    else
    {
      // Job not executing — verify it exists and is in a cancellable phase.
      auto job = db_.load_job (job_id);
      if (!job)
      {
        reply_error (identity, request_id, -32020, "Not found");
        return;
      }
      static const std::unordered_set<std::string> cancellable
        = {"planning", "executing", "repairing"};
      if (!cancellable.count (job->phase))
      {
        reply_error (identity, request_id, -32022, "Invalid state");
        return;
      }
    }

    // Mark as cancelled — distinct terminal state, not failed.
    db_.update_job_phase (TaskId (job_id), "cancelled");
    notify ("job.phase_changed",
            R"({"job_id":")" + job_id + R"(","new_phase":"cancelled"})");
    spdlog::info ("[orchestrator] job {} cancelled", job_id);
    reply_ok (identity, request_id, R"({"ok":true})");
  }

  // ---------------------------------------------------------------------------
  // Command: review.approve / review.reject
  // ---------------------------------------------------------------------------

  void Orchestrator::cmd_review_approve (const std::string &params_json,
                                         const std::string &identity,
                                         const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("review_id") || !params["review_id"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    // Human review approval — forward to ForgeCoordinator via MasterDecision.
    // ForgeCoordinator handles the actual state transition.
    OrchestratorEvent oe;
    oe.kind = OrchestratorEvent::Kind::MasterDecision;
    oe.payload_json = R"({"type":"review_approve","review_id":")"
                      + std::string (params["review_id"].GetString ())
                      + R"("})";
    on_message (
      std::move (oe)); // self-dispatch (already on Orchestrator thread)
    reply_ok (identity, request_id, R"({"ok":true})");
  }

  void Orchestrator::cmd_review_reject (const std::string &params_json,
                                        const std::string &identity,
                                        const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("review_id") || !params["review_id"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    reply_ok (identity, request_id, R"({"ok":true})");
  }

  // ---------------------------------------------------------------------------
  // Command: worker.list
  // ---------------------------------------------------------------------------

  void Orchestrator::cmd_worker_list (const std::string & /*params_json*/,
                                      const std::string &identity,
                                      const std::string &request_id)
  {
    auto agents = db_.load_enabled_agents ();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("workers");
    w.StartArray ();
    for (const auto &a : agents)
    {
      if (a.role != "worker")
        continue;
      w.StartObject ();
      w.Key ("id");
      w.String (a.id.c_str ());
      w.Key ("tier");
      w.String ("tier0");
      w.Key ("provenance");
      w.String ("manual");
      w.Key ("enabled");
      w.Bool (true);
      w.Key ("capabilities");
      w.StartArray ();
      w.EndArray ();
      w.Key ("registered_at");
      w.Int64 (0);
      w.EndObject ();
    }
    w.EndArray ();
    w.EndObject ();
    reply_ok (identity, request_id, buf.GetString ());
  }

  // ---------------------------------------------------------------------------
  // Command: adviser.list
  // ---------------------------------------------------------------------------

  void Orchestrator::cmd_adviser_list (const std::string & /*params_json*/,
                                       const std::string &identity,
                                       const std::string &request_id)
  {
    auto agents = db_.load_enabled_agents ();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("advisers");
    w.StartArray ();
    for (const auto &a : agents)
    {
      if (a.role != "adviser")
        continue;
      w.StartObject ();
      w.Key ("id");
      w.String (a.id.c_str ());
      w.Key ("description");
      w.String ("");
      w.Key ("skill_path");
      w.String (a.binary_path.c_str ());
      w.Key ("model");
      w.String ("");
      w.Key ("active");
      w.Bool (false);
      w.EndObject ();
    }
    w.EndArray ();
    w.EndObject ();
    reply_ok (identity, request_id, buf.GetString ());
  }

  // ---------------------------------------------------------------------------
  // Command: worker.register
  // ---------------------------------------------------------------------------

  void Orchestrator::cmd_worker_register (const std::string &params_json,
                                          const std::string &identity,
                                          const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("path") || !params["path"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    // TODO: validate manifest and register worker via Registry.
    reply_ok (identity, request_id, R"({"ok":true})");
  }

  // ---------------------------------------------------------------------------
  // WorkerDone
  // ---------------------------------------------------------------------------

  void Orchestrator::handle_worker_done (const OrchestratorEvent &ev)
  {
    rapidjson::Document doc;
    if (doc.Parse (ev.payload_json.c_str ()).HasParseError ())
      return;

    const std::string run_id
      = doc.HasMember ("run_id") && doc["run_id"].IsString ()
          ? doc["run_id"].GetString ()
          : "";
    const std::string job_dir
      = doc.HasMember ("job_dir") && doc["job_dir"].IsString ()
          ? doc["job_dir"].GetString ()
          : "";
    const int exit_code
      = doc.HasMember ("exit_code") && doc["exit_code"].IsInt ()
          ? doc["exit_code"].GetInt ()
          : 0;

    // Find which job owns this run_id.
    std::string job_id;
    for (const auto &[jid, job] : active_jobs_)
      if (job.current_run_id == run_id)
      {
        job_id = jid;
        break;
      }

    if (job_id.empty ())
    {
      spdlog::warn ("[orchestrator] WorkerDone: unknown run_id {}", run_id);
      return;
    }

    on_step_complete (job_id, run_id, exit_code, job_dir);
  }

  // ---------------------------------------------------------------------------
  // WorkerFailed
  // ---------------------------------------------------------------------------

  void Orchestrator::handle_worker_failed (const OrchestratorEvent &ev)
  {
    rapidjson::Document doc;
    if (doc.Parse (ev.payload_json.c_str ()).HasParseError ())
      return;

    const std::string run_id
      = doc.HasMember ("run_id") && doc["run_id"].IsString ()
          ? doc["run_id"].GetString ()
          : "";
    const int exit_code
      = doc.HasMember ("exit_code") && doc["exit_code"].IsInt ()
          ? doc["exit_code"].GetInt ()
          : -1;

    std::string job_id;
    for (const auto &[jid, job] : active_jobs_)
      if (job.current_run_id == run_id)
      {
        job_id = jid;
        break;
      }

    if (job_id.empty ())
    {
      spdlog::warn ("[orchestrator] WorkerFailed: unknown run_id {}", run_id);
      return;
    }

    on_step_failed (job_id, run_id, exit_code);
  }

  // ---------------------------------------------------------------------------
  // AdviserDone / AdviserFailed
  // ---------------------------------------------------------------------------

  void Orchestrator::handle_adviser_done (const OrchestratorEvent &ev)
  {
    // Adviser produced a plan — forward to Master for review.
    MasterEvent me;
    me.kind = MasterEvent::Kind::JobSubmit;
    me.job_id = ev.job_id;
    me.payload_json = ev.payload_json;
    send_to_master_ (std::move (me));
  }

  void Orchestrator::handle_adviser_failed (const OrchestratorEvent &ev)
  {
    MasterEvent me;
    me.kind = MasterEvent::Kind::AdviserFailed;
    me.job_id = ev.job_id;
    me.payload_json = ev.payload_json;
    send_to_master_ (std::move (me));
  }

  // ---------------------------------------------------------------------------
  // MasterDecision
  // ---------------------------------------------------------------------------

  void Orchestrator::handle_master_decision (const OrchestratorEvent &ev)
  {
    rapidjson::Document doc;
    if (doc.Parse (ev.payload_json.c_str ()).HasParseError ())
      return;

    if (!doc.HasMember ("type") || !doc["type"].IsString ())
      return;

    const std::string type = doc["type"].GetString ();

    if (type == "spawn_adviser")
    {
      // Master has selected an Adviser — spawn it as a detached thread.
      // Thread calls LlmProxy, produces plan_ready or AdviserFailed.
      // ADR-018: skill.md is the system prompt; Adviser owns its session.
      const std::string job_id
        = doc.HasMember ("job_id") && doc["job_id"].IsString ()
            ? doc["job_id"].GetString ()
            : "";
      const std::string adviser_id
        = doc.HasMember ("adviser_id") && doc["adviser_id"].IsString ()
            ? doc["adviser_id"].GetString ()
            : "";
      const std::string goal
        = doc.HasMember ("goal") && doc["goal"].IsString ()
            ? doc["goal"].GetString ()
            : "";

      if (job_id.empty () || adviser_id.empty ())
      {
        spdlog::error (
          "[orchestrator] spawn_adviser: missing job_id or adviser_id");
        return;
      }

      spdlog::info ("[orchestrator] spawning adviser {} for job {}",
                    adviser_id, job_id);

      // Read skill.md as system prompt (ADR-018).
      const char *home_env = std::getenv ("HOME");
      const std::string skill_path
        = std::string (home_env ? home_env : "")
          + "/.agentos/advisers/" + adviser_id + "/skill.md";
      std::string system_prompt;
      {
        std::ifstream f (skill_path);
        if (f)
          system_prompt.assign (std::istreambuf_iterator<char> (f),
                                std::istreambuf_iterator<char> ());
        else
          spdlog::warn (
            "[orchestrator] skill.md not found for adviser {}, using empty "
            "system prompt",
            adviser_id);
      }

      // Detach LLM thread — never blocks Orchestrator event loop.
      // Captures by value; `this` is safe (Orchestrator outlives all threads).
      std::thread (
        [this, job_id, goal,
         system_prompt = std::move (system_prompt)] () mutable
        {
          // LlmProxy has no complete(); LlmClient wraps it (ADR-017).
          LlmClient client (llm_, config_.llm);

          LlmRequest req;
          req.system_prompt = std::move (system_prompt);
          req.user_prompt
            = "Decompose the following goal into an ordered list of steps. "
              "Respond ONLY with a JSON object — no markdown, no prose — in "
              "this exact shape: "
              "{\"steps\":[{\"id\":\"<uuid>\",\"command\":\"<capability>\","
              "\"description\":\"<what this step does>\"},...]}. "
              "Goal: "
              + goal;
          req.max_tokens = 1024;

          auto result = client.complete (req);

          OrchestratorEvent ev;
          ev.job_id = job_id;

          if (!result.ok)
          {
            spdlog::error (
              "[orchestrator] adviser LLM call failed for job {}: {}", job_id,
              result.error);
            ev.kind = OrchestratorEvent::Kind::AdviserFailed;
            ev.payload_json = R"({"job_id":")" + job_id
                              + R"(","reason":"LLM call failed"})";
            enqueue (std::move (ev));
            return;
          }

          // Parse steps JSON from LLM response.
          rapidjson::Document resp;
          const std::string &content = result.value.content;
          if (resp.Parse (content.c_str ()).HasParseError ()
              || !resp.IsObject () || !resp.HasMember ("steps")
              || !resp["steps"].IsArray ())
          {
            spdlog::error (
              "[orchestrator] adviser produced invalid plan for job {}: {}",
              job_id, content);
            ev.kind = OrchestratorEvent::Kind::AdviserFailed;
            ev.payload_json = R"({"job_id":")" + job_id
                              + R"(","reason":"invalid plan JSON"})";
            enqueue (std::move (ev));
            return;
          }

          // Assign stable UUIDs to any steps that lack one.
          for (auto &s : resp["steps"].GetArray ())
          {
            if (!s.HasMember ("id") || !s["id"].IsString ()
                || std::string (s["id"].GetString ()).empty ())
            {
              s.AddMember (
                rapidjson::Value ("id", resp.GetAllocator ()),
                rapidjson::Value (new_uuid ().c_str (), resp.GetAllocator ()),
                resp.GetAllocator ());
            }
          }

          // Build plan_ready payload — enqueue directly as MasterDecision
          // so handle_master_decision picks it up on the Orchestrator thread.
          rapidjson::StringBuffer buf;
          rapidjson::Writer<rapidjson::StringBuffer> w (buf);
          w.StartObject ();
          w.Key ("type");
          w.String ("plan_ready");
          w.Key ("job_id");
          w.String (job_id.c_str ());
          w.Key ("job_type");
          w.String ("oneshot");
          w.Key ("steps");
          resp["steps"].Accept (w);
          w.EndObject ();

          ev.kind = OrchestratorEvent::Kind::MasterDecision;
          ev.payload_json = buf.GetString ();
          enqueue (std::move (ev));
        })
        .detach ();

      return;
    }

    if (type == "plan_ready")
    {
      // Master has produced a plan — build ActiveJob and start pipeline.
      const std::string job_id
        = doc.HasMember ("job_id") && doc["job_id"].IsString ()
            ? doc["job_id"].GetString ()
            : "";
      if (job_id.empty ())
        return;

      // Parse pipeline steps from payload.
      ActiveJob job;
      job.job_id = job_id;
      job.type = (doc.HasMember ("job_type") && doc["job_type"].IsString ())
                   ? doc["job_type"].GetString ()
                   : "oneshot";

      if (doc.HasMember ("steps") && doc["steps"].IsArray ())
      {
        int order = 0;
        for (const auto &s : doc["steps"].GetArray ())
        {
          ActiveStep as;
          if (s.HasMember ("id") && s["id"].IsString ())
            as.step.id = s["id"].GetString ();
          if (s.HasMember ("command") && s["command"].IsString ())
            as.step.command = s["command"].GetString ();
          if (s.HasMember ("description") && s["description"].IsString ())
            as.step.description = s["description"].GetString ();
          // Persist step to DB so job.status can display it (ADR-022).
          db_.store_pipeline_task (TaskId (job_id), as.step, order++);
          job.pending_steps.push_back (std::move (as));
        }
      }

      db_.update_job_phase (TaskId (job_id), "executing");
      active_jobs_[job_id] = std::move (job);
      dispatch_next_step (active_jobs_[job_id]);
    }
    else if (type == "trigger_forge")
    {
      // Launch Forge pipeline to generate a Worker for the missing capability.
      // ADR-019: ForgeCoordinator owns the state machine; Orchestrator just
      // creates the DB record and posts the request.
      const std::string job_id
        = doc.HasMember ("job_id") && doc["job_id"].IsString ()
            ? doc["job_id"].GetString ()
            : "";
      const std::string command
        = doc.HasMember ("command") && doc["command"].IsString ()
            ? doc["command"].GetString ()
            : "";

      if (job_id.empty () || command.empty ())
      {
        spdlog::error ("[orchestrator] trigger_forge: missing job_id or command");
        finish_job (job_id, false, "trigger_forge: missing fields");
        return;
      }

      // Build requirement_json (ADR-019 Code Writer input contract).
      rapidjson::StringBuffer req_buf;
      rapidjson::Writer<rapidjson::StringBuffer> rw (req_buf);
      rw.StartObject ();
      rw.Key ("description");
      rw.String (("Implement capability: " + command).c_str ());
      rw.Key ("method");
      rw.String (command.c_str ());
      rw.Key ("input_schema");
      rw.StartObject (); rw.EndObject ();
      rw.Key ("output_schema");
      rw.StartObject (); rw.EndObject ();
      rw.EndObject ();
      const std::string requirement_json = req_buf.GetString ();

      // Create and persist the forge_pipeline_jobs row (persist-before-act).
      const std::string forge_job_id = new_uuid ();
      ForgePipelineJob fpj;
      fpj.id               = forge_job_id;
      fpj.task_id          = job_id;
      fpj.status           = ForgeStatus::drafting;
      fpj.requirement_json = requirement_json;
      fpj.attempt          = 0;
      fpj.max_attempts     = 3;
      fpj.created_at       = now_unix ();
      fpj.updated_at       = now_unix ();
      db_.store_forge_pipeline_job (fpj);

      spdlog::info ("[orchestrator] trigger_forge job_id={} command={} "
                    "forge_job_id={}",
                    job_id, command, forge_job_id);

      // Post to ForgeCoordinator — returns immediately.
      forge::ForgeRequest freq;
      freq.forge_job_id     = forge_job_id;
      freq.task_id          = job_id;
      freq.requirement_json = requirement_json;
      freq.feedback         = "";
      freq.attempt          = 0;
      freq.max_attempts     = 3;
      forge_.post (std::move (freq));
    }
    else if (type == "forge_complete")
    {
      // ForgeCoordinator completed — retry the waiting step.
      const std::string task_id
        = doc.HasMember ("task_id") && doc["task_id"].IsString ()
            ? doc["task_id"].GetString ()
            : "";
      const int outcome = doc.HasMember ("outcome") && doc["outcome"].IsInt ()
                            ? doc["outcome"].GetInt ()
                            : -1;
      const std::string worker_id
        = doc.HasMember ("worker_id") && doc["worker_id"].IsString ()
            ? doc["worker_id"].GetString ()
            : "";

      spdlog::info ("[orchestrator] forge_complete task_id={} outcome={} "
                    "worker_id={} active_jobs_size={}",
                    task_id, outcome, worker_id, active_jobs_.size ());

      if (outcome == 0) // ForgeResult::Outcome::promoted
      {
        auto it = active_jobs_.find (task_id);
        if (it != active_jobs_.end ())
        {
          spdlog::info ("[orchestrator] forge_complete: resuming job {}",
                        task_id);
          dispatch_next_step (it->second);
        }
        else
        {
          spdlog::error ("[orchestrator] forge_complete: job {} not in "
                         "active_jobs_ — cannot resume",
                         task_id);
        }
      }
      else
      {
        finish_job (task_id, false, "forge pipeline failed");
      }
    }
    else if (type == "job_failed")
    {
      const std::string job_id
        = doc.HasMember ("job_id") && doc["job_id"].IsString ()
            ? doc["job_id"].GetString ()
            : "";
      const std::string reason
        = doc.HasMember ("reason") && doc["reason"].IsString ()
            ? doc["reason"].GetString ()
            : "unknown";
      finish_job (job_id, false, reason);
    }
  }

  // ---------------------------------------------------------------------------
  // TimerFired
  // ---------------------------------------------------------------------------

  void Orchestrator::handle_timer_fired (const OrchestratorEvent &ev)
  {
    rapidjson::Document doc;
    if (doc.Parse (ev.payload_json.c_str ()).HasParseError ())
      return;

    if (!doc.HasMember ("kind") || !doc["kind"].IsString ())
      return;

    const std::string kind = doc["kind"].GetString ();

    if (kind == "scheduled_job_fire")
    {
      // Create a new oneshot job instance for the scheduled template.
      const std::string goal = doc.HasMember ("goal") && doc["goal"].IsString ()
                                 ? doc["goal"].GetString ()
                                 : "";
      if (!goal.empty ())
      {
        OrchestratorEvent submit;
        submit.kind = OrchestratorEvent::Kind::GatewayInbound;
        submit.payload_json
          = R"({"method":"job.submit","key":"","id":"timer","params":{"goal":")"
            + goal + R"("}})";
        on_message (std::move (submit));
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Pipeline execution
  // ---------------------------------------------------------------------------

  void Orchestrator::dispatch_next_step (ActiveJob &job)
  {
    if (job.pending_steps.empty ())
    {
      finish_job (job.job_id, true);
      return;
    }

    ActiveStep &step = job.pending_steps.front ();

    // Look up Worker for this step's command.
    auto worker = registry_.find_worker_for_command (step.step.command);
    if (!worker)
    {
      spdlog::info ("[orchestrator] no worker for command '{}', asking Master",
                    step.step.command);
      MasterEvent me;
      me.kind = MasterEvent::Kind::WorkerExhausted;
      me.job_id = job.job_id;
      me.payload_json = R"({"job_id":")" + job.job_id + R"(","command":")"
                        + step.step.command + R"("})";
      send_to_master_ (std::move (me));
      return;
    }

    // Generate run_id and build DispatchRequest.
    const std::string run_id = new_uuid ();
    step.run_id = run_id;

    DispatchRequest req;
    req.run_id = run_id;
    req.step_id = step.step.id;
    req.worker_id = worker->id.value ();
    req.binary_path = worker->binary_path;
    req.task_json = R"({"job_id":")" + job.job_id + R"(","step_id":")"
                    + step.step.id + R"("})";
    req.network = false; // from manifest; TODO: load from Registry

    // Persist worker_run before fork (ADR-022: persist before act).
    WorkerRun run;
    run.run_id = run_id;
    run.worker_id = worker->id.value ();
    run.pid = 0; // filled after fork
    run.started_at = static_cast<int64_t> (
      std::chrono::duration_cast<std::chrono::seconds> (
        std::chrono::system_clock::now ().time_since_epoch ())
        .count ());
    run.status = WorkerStatus::running;
    run.layer_path = {};
    run.log_path = {};
    db_.insert_worker_run (run);

    auto result = dispatcher_.fork_exec (req);
    if (!result.ok)
    {
      spdlog::error ("[orchestrator] fork_exec failed for job {}: {}",
                     job.job_id, result.error);
      on_step_failed (job.job_id, run_id, -1);
      return;
    }

    // Update DB with actual pid, job_dir, log_path.
    run.pid = result.pid;
    run.layer_path = result.job_dir;
    run.log_path = result.log_path;
    db_.update_worker_run (run);

    job.current_run_id = run_id;
    step.job_dir = result.job_dir;

    spdlog::info ("[orchestrator] dispatched step {} run_id={} pid={}",
                  step.step.id, run_id, result.pid);
  }

  void Orchestrator::on_step_complete (const std::string &job_id,
                                       const std::string &run_id, int exit_code,
                                       const std::string &job_dir)
  {
    auto it = active_jobs_.find (job_id);
    if (it == active_jobs_.end ())
      return;

    ActiveJob &job = it->second;

    // Collect result.
    auto collected = dispatcher_.collect (run_id, job_dir, exit_code);
    if (!collected.ok)
    {
      spdlog::warn ("[orchestrator] collect failed for run_id={}: {}", run_id,
                    collected.error);
      on_step_failed (job_id, run_id, exit_code);
      return;
    }

    // Store result in DB.
    if (!job.pending_steps.empty ())
    {
      db_.update_step_result (job.pending_steps.front ().step.id,
                              collected.result_json);
    }

    // Advance pipeline.
    job.pending_steps.pop_front ();
    job.current_run_id.clear ();

    // Notify Gateway.
    notify ("job.step_changed",
            R"({"job_id":")" + job_id + R"(","status":"done"})");

    dispatch_next_step (job);
  }

  void Orchestrator::on_step_failed (const std::string &job_id,
                                     const std::string &run_id, int exit_code)
  {
    spdlog::warn ("[orchestrator] step failed job_id={} run_id={} exit={}",
                  job_id, run_id, exit_code);

    auto it = active_jobs_.find (job_id);
    if (it == active_jobs_.end ())
      return;

    // Try another Worker with the same capability.
    ActiveJob &job = it->second;
    if (!job.pending_steps.empty ())
    {
      job.pending_steps.front ().attempts++;
      if (job.pending_steps.front ().attempts < 3)
      {
        dispatch_next_step (job);
        return;
      }
    }

    // Exhausted retries — escalate to Master.
    MasterEvent me;
    me.kind = MasterEvent::Kind::WorkerExhausted;
    me.job_id = job_id;
    send_to_master_ (std::move (me));
  }

  void Orchestrator::finish_job (const std::string &job_id, bool success,
                                 const std::string &error)
  {
    db_.update_job_phase (TaskId (job_id), success ? "done" : "failed");

    if (!success && !error.empty ())
      db_.update_job_error (job_id, error);

    active_jobs_.erase (job_id);

    notify ("job.phase_changed", R"({"job_id":")" + job_id
            + R"(","new_phase":")"
            + (success ? "done" : "failed") + R"("})");

    spdlog::info ("[orchestrator] job {} {}", job_id,
                  success ? "done" : ("failed: " + error));
  }

  // ---------------------------------------------------------------------------
  // Response helpers
  // ---------------------------------------------------------------------------

  void Orchestrator::reply_ok (const std::string &identity,
                               const std::string &request_id,
                               const std::string &result_json)
  {
    GatewayEvent ev;
    ev.kind = GatewayEvent::Kind::Outbound;
    ev.outbound.identity = identity;
    ev.outbound.message = make_response (request_id, result_json);
    send_to_gateway_ (std::move (ev));
  }

  void Orchestrator::reply_error (const std::string &identity,
                                  const std::string &request_id, int code,
                                  const std::string &message)
  {
    GatewayEvent ev;
    ev.kind = GatewayEvent::Kind::Outbound;
    ev.outbound.identity = identity;
    ev.outbound.message = make_error_response (request_id, code, message);
    send_to_gateway_ (std::move (ev));
  }

  void Orchestrator::notify (const std::string &method,
                             const std::string &params_json)
  {
    GatewayEvent ev;
    ev.kind = GatewayEvent::Kind::Outbound;
    ev.outbound.identity = ""; // broadcast
    ev.outbound.message = make_notification (method, params_json);
    send_to_gateway_ (std::move (ev));
  }

  std::string Orchestrator::new_uuid ()
  {
    return gen_uuid ();
  }

  // ---------------------------------------------------------------------------
  // ADR-028: cred.* JSON-RPC handlers
  // ---------------------------------------------------------------------------

  void Orchestrator::cmd_cred_submit (const std::string &params_json,
                                      const std::string &identity,
                                      const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("user_id") || !params["user_id"].IsString ()
        || !params.HasMember ("provider") || !params["provider"].IsString ()
        || !params.HasMember ("token") || !params["token"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }

    std::string user_id = params["user_id"].GetString ();
    std::string provider = params["provider"].GetString ();
    std::string token = params["token"].GetString ();

    std::optional<std::string> refresh;
    if (params.HasMember ("refresh_token")
        && params["refresh_token"].IsString ())
      refresh = params["refresh_token"].GetString ();

    std::optional<int64_t> expires;
    if (params.HasMember ("expires_at") && params["expires_at"].IsInt64 ())
      expires = params["expires_at"].GetInt64 ();

    auto result
      = cred_vault_.submit (user_id, provider, token, refresh, expires);
    if (!result)
    {
      reply_error (identity, request_id, -32030, "cred.submit failed");
      return;
    }
    reply_ok (identity, request_id,
              std::string ("{\"credential_id\":\"") + *result + "\"}");
  }

  void Orchestrator::cmd_cred_revoke (const std::string &params_json,
                                      const std::string &identity,
                                      const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("user_id") || !params["user_id"].IsString ()
        || !params.HasMember ("provider") || !params["provider"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    bool ok = cred_vault_.revoke (params["user_id"].GetString (),
                                  params["provider"].GetString ());
    reply_ok (identity, request_id,
              std::string ("{\"ok\":") + (ok ? "true" : "false") + "}");
  }

  void Orchestrator::cmd_cred_grant (const std::string &params_json,
                                     const std::string &identity,
                                     const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("worker_id") || !params["worker_id"].IsString ()
        || !params.HasMember ("provider") || !params["provider"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    auto res = cred_vault_.grant (
      params["worker_id"].GetString (), params["provider"].GetString (),
      "admin"); // granted_by: use actual key id in production
    if (!res)
    {
      reply_error (identity, request_id, -32030, res.error ());
      return;
    }
    reply_ok (identity, request_id,
              std::string ("{\"grant_id\":\"") + *res + "\"}");
  }

  void Orchestrator::cmd_cred_revoke_grant (const std::string &params_json,
                                            const std::string &identity,
                                            const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("grant_id") || !params["grant_id"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    bool ok = cred_vault_.revoke_grant (params["grant_id"].GetString ());
    reply_ok (identity, request_id,
              std::string ("{\"ok\":") + (ok ? "true" : "false") + "}");
  }

  void Orchestrator::cmd_cred_list (const std::string &params_json,
                                    const std::string &identity,
                                    const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("user_id") || !params["user_id"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    auto list = cred_vault_.list (params["user_id"].GetString ());

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("credentials");
    w.StartArray ();
    for (const auto &c : list)
    {
      w.StartObject ();
      w.Key ("id");
      w.String (c.id.c_str ());
      w.Key ("provider");
      w.String (c.provider.c_str ());
      if (c.expires_at)
      {
        w.Key ("expires_at");
        w.Int64 (*c.expires_at);
      }
      w.Key ("updated_at");
      w.Int64 (c.updated_at);
      w.EndObject ();
    }
    w.EndArray ();
    w.EndObject ();
    reply_ok (identity, request_id, buf.GetString ());
  }

  void Orchestrator::cmd_cred_audit (const std::string &params_json,
                                     const std::string &identity,
                                     const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    std::optional<std::string> user_id, job_id, provider;
    if (params.HasMember ("user_id") && params["user_id"].IsString ())
      user_id = params["user_id"].GetString ();
    if (params.HasMember ("job_id") && params["job_id"].IsString ())
      job_id = params["job_id"].GetString ();
    if (params.HasMember ("provider") && params["provider"].IsString ())
      provider = params["provider"].GetString ();
    int limit = 50;
    if (params.HasMember ("limit") && params["limit"].IsInt ())
      limit = params["limit"].GetInt ();

    auto entries = cred_vault_.audit (user_id, job_id, provider, limit);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("entries");
    w.StartArray ();
    for (const auto &e : entries)
    {
      w.StartObject ();
      w.Key ("id");
      w.String (e.id.c_str ());
      w.Key ("credential_id");
      w.String (e.credential_id.c_str ());
      w.Key ("user_id");
      w.String (e.user_id.c_str ());
      w.Key ("worker_id");
      w.String (e.worker_id.c_str ());
      w.Key ("job_id");
      w.String (e.job_id.c_str ());
      w.Key ("step_id");
      w.String (e.step_id.c_str ());
      w.Key ("run_id");
      w.String (e.run_id.c_str ());
      w.Key ("action");
      w.String (e.action.c_str ());
      if (e.reason)
      {
        w.Key ("reason");
        w.String (e.reason->c_str ());
      }
      w.Key ("timestamp");
      w.Int64 (e.timestamp);
      w.EndObject ();
    }
    w.EndArray ();
    w.EndObject ();
    reply_ok (identity, request_id, buf.GetString ());
  }

  void Orchestrator::cmd_vault_rekey (const std::string & /*params*/,
                                      const std::string &identity,
                                      const std::string &request_id)
  {
    auto res = cred_vault_.rekey ();
    if (!res)
      reply_error (identity, request_id, -32030, "vault.rekey failed");
    else
      reply_ok (identity, request_id, "{\"ok\":true}");
  }

  // ---------------------------------------------------------------------------
  // ADR-029: user.* JSON-RPC handlers (admin only)
  // ---------------------------------------------------------------------------

  void Orchestrator::cmd_user_register (const std::string &params_json,
                                        const std::string &identity,
                                        const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("user_id") || !params["user_id"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    auto res = user_manager_.register_user (params["user_id"].GetString ());
    if (!res)
    {
      reply_error (identity, request_id, -32030, res.error ());
      return;
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("user_id");
    w.String (res->id.c_str ());
    w.Key ("created_at");
    w.Int64 (res->created_at);
    w.EndObject ();
    reply_ok (identity, request_id, buf.GetString ());
  }

  void Orchestrator::cmd_user_list (const std::string &params_json,
                                    const std::string &identity,
                                    const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    std::optional<bool> enabled_filter;
    if (params.HasMember ("enabled") && params["enabled"].IsBool ())
      enabled_filter = params["enabled"].GetBool ();

    int limit = 50;
    int offset = 0;
    if (params.HasMember ("limit") && params["limit"].IsInt ())
      limit = params["limit"].GetInt ();
    if (params.HasMember ("offset") && params["offset"].IsInt ())
      offset = params["offset"].GetInt ();

    auto users = user_manager_.list_users (enabled_filter, limit, offset);
    if (!users)
    {
      reply_error (identity, request_id, -32030, users.error ());
      return;
    }
    auto total = user_manager_.count_users (enabled_filter);
    if (!total)
    {
      reply_error (identity, request_id, -32030, total.error ());
      return;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("users");
    w.StartArray ();
    for (const auto &u : *users)
    {
      w.StartObject ();
      w.Key ("id");
      w.String (u.id.c_str ());
      w.Key ("enabled");
      w.Bool (u.enabled);
      w.Key ("created_at");
      w.Int64 (u.created_at);
      w.EndObject ();
    }
    w.EndArray ();
    w.Key ("total");
    w.Int (*total);
    w.EndObject ();
    reply_ok (identity, request_id, buf.GetString ());
  }

  void Orchestrator::cmd_user_enable (const std::string &params_json,
                                      const std::string &identity,
                                      const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("user_id") || !params["user_id"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    auto res = user_manager_.enable_user (params["user_id"].GetString ());
    if (!res)
    {
      reply_error (identity, request_id, -32020, res.error ());
      return;
    }
    reply_ok (identity, request_id, "{\"ok\":true}");
  }

  void Orchestrator::cmd_user_disable (const std::string &params_json,
                                       const std::string &identity,
                                       const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("user_id") || !params["user_id"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    auto res = user_manager_.disable_user (params["user_id"].GetString ());
    if (!res)
    {
      reply_error (identity, request_id, -32020, res.error ());
      return;
    }
    reply_ok (identity, request_id, "{\"ok\":true}");
  }

  void Orchestrator::cmd_user_profile (const std::string &params_json,
                                       const std::string &identity,
                                       const std::string &request_id)
  {
    rapidjson::Document params;
    if (params.Parse (params_json.c_str ()).HasParseError ()
        || !params.HasMember ("user_id") || !params["user_id"].IsString ())
    {
      reply_error (identity, request_id, -32602, "Invalid params");
      return;
    }
    auto res = user_manager_.get_profile (params["user_id"].GetString ());
    if (!res)
    {
      reply_error (identity, request_id, -32020, res.error ());
      return;
    }
    const auto &p = *res;
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("user_id");
    w.String (p.user_id.c_str ());
    w.Key ("first_seen");
    w.Int64 (p.first_seen);
    w.Key ("last_seen");
    if (p.last_seen)
      w.Int64 (*p.last_seen);
    else
      w.Null ();
    w.Key ("total_jobs");
    w.Int (p.total_jobs);
    w.Key ("successful_jobs");
    w.Int (p.successful_jobs);
    w.Key ("failed_jobs");
    w.Int (p.failed_jobs);
    w.Key ("connected_providers");
    w.StartArray ();
    for (const auto &prov : p.connected_providers)
      w.String (prov.c_str ());
    w.EndArray ();
    w.EndObject ();
    reply_ok (identity, request_id, buf.GetString ());
  }

} // namespace agentos
