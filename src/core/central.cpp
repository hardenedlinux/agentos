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
 * agentos/central.cpp
 *
 * ADR-024: Central — actor container, startup/shutdown, callback injection.
 *
 * Outbound gateway path:
 *   send_to_gateway() → gateway_push() → gateway_.enqueue_outbound()
 *   Gateway poll thread drains and sends — no inproc, no extra thread.
 */

#include "agentos/central.h"
#include "agentos/home_init.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <csignal>
#include <sys/prctl.h>

namespace agentos
{

  Central::Central (const Config &config)
    : config_ (config), db_ ((agentos_home () / "agentos.db").string ()),
      llm_proxy_ (resolve_concurrency (config.llm.max_concurrent),
                  config.llm.timeout_s),
      llm_client_ (llm_proxy_, config_.llm), registry_ (), dispatcher_ (),
      forge_coordinator_ (
        db_, llm_proxy_, registry_,
        [this] (forge::ForgeResult result)
        { send_to_orchestrator (make_forge_complete_event (result)); }),
      cred_vault_ (db_, config_.vault),
      gateway_ (zmq_ctx_,
                [this] (GatewayInbound msg)
                {
                  OrchestratorEvent ev;
                  ev.kind         = OrchestratorEvent::Kind::GatewayInbound;
                  ev.payload_json = std::move (msg.message);
                  ev.identity     = std::move (msg.identity);
                  send_to_orchestrator (std::move (ev));
                }),
      orchestrator_ (
        db_, llm_proxy_, registry_, dispatcher_, forge_coordinator_, config_,
        cred_vault_,
        [this] (MasterEvent msg) { send_to_master (std::move (msg)); },
        [this] (GatewayEvent msg) { send_to_gateway (std::move (msg)); }),
      master_ (llm_client_, registry_,
               [this] (OrchestratorEvent msg)
               { send_to_orchestrator (std::move (msg)); }),
      periodic_ (
        db_, dispatcher_, config_,
        [this] (OrchestratorEvent msg)
        { send_to_orchestrator (std::move (msg)); },
        [this] (MasterEvent msg) { send_to_master (std::move (msg)); },
        [this] (const std::string &payload) { gateway_push (payload); })
  {
  }

  Central::~Central ()
  {
    stop ();
  }

  void Central::run ()
  {
    spdlog::info ("[central] starting");

    if (!db_.open ())
    {
      spdlog::critical ("[central] failed to open database — aborting");
      return;
    }
    registry_.init (db_);

    if (prctl (PR_SET_DUMPABLE, 0) != 0)
      spdlog::warn ("[central] prctl(PR_SET_DUMPABLE, 0) failed — core dumps "
                    "may expose vault key");

    {
      auto vault_result = cred_vault_.start ();
      if (!vault_result)
      {
        spdlog::critical ("[central] cred_vault start failed: {}",
                          vault_result.error ());
        return;
      }
    }

    orchestrator_.init ();
    orchestrator_.start ();

    master_.start ();

    periodic_.init ();
    periodic_.start ();

    // Gateway last — external interface opens only when all actors are ready.
    gateway_.start ();

    spdlog::info ("[central] all actors started");

    while (!stop_requested_)
      std::this_thread::sleep_for (std::chrono::milliseconds (100));

    spdlog::info ("Agentos is trying to stop...");
    stop_all ();
    spdlog::info ("done.");
  }

  void Central::stop ()
  {
    stop_requested_ = true;
  }

  void Central::stop_all ()
  {
    spdlog::info ("[central] stopping");

    gateway_.stop ();
    periodic_.stop ();
    master_.stop ();
    orchestrator_.stop ();

    cred_vault_.stop ();
    cred_vault_.clear ();

    forge_coordinator_.stop ();
    db_.close ();

    spdlog::info ("[central] stopped");
  }

  void Central::send_to_orchestrator (OrchestratorEvent msg)
  {
    orchestrator_.enqueue (std::move (msg));
  }

  void Central::send_to_master (MasterEvent msg)
  {
    master_.enqueue (std::move (msg));
  }

  void Central::send_to_gateway (GatewayEvent msg)
  {
    if (msg.kind == GatewayEvent::Kind::Outbound)
      gateway_push (msg.outbound.message, msg.outbound.identity);
  }

  void Central::send_to_periodic (PeriodicControl msg)
  {
    periodic_.enqueue (std::move (msg));
  }

  void Central::gateway_push (const std::string &payload,
                              const std::string &identity)
  {
    gateway_.enqueue_outbound (identity, payload);
  }

  OrchestratorEvent
  Central::make_forge_complete_event (const forge::ForgeResult &result)
  {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("type");         w.String ("forge_complete");
    w.Key ("forge_job_id"); w.String (result.forge_job_id.c_str ());
    w.Key ("task_id");      w.String (result.task_id.c_str ());
    w.Key ("outcome");      w.Int (static_cast<int> (result.outcome));
    w.Key ("worker_id");    w.String (result.worker_id.c_str ());
    w.Key ("review_id");    w.String (result.review_id.c_str ());
    w.Key ("error");        w.String (result.error.c_str ());
    w.EndObject ();

    OrchestratorEvent ev;
    ev.kind         = OrchestratorEvent::Kind::MasterDecision;
    ev.payload_json = buf.GetString ();
    return ev;
  }

} // namespace agentos
