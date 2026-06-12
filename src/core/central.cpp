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
 * Startup order:
 *   db → llm_proxy → registry → forge_coordinator → orchestrator →
 *   master → periodic → gateway
 *
 * Shutdown order (reverse):
 *   gateway → periodic → master → orchestrator →
 *   forge_coordinator → llm_proxy → db
 */

#include "agentos/central.h"
#include "agentos/home_init.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <csignal>

namespace agentos
{

// ---------------------------------------------------------------------------
// Construction — initialise shared services and wire callbacks
// ---------------------------------------------------------------------------

Central::Central (const Config &config)
    : config_ (config),
      db_ ((agentos_home () / "agentos.db").string ()),
      llm_proxy_ (resolve_concurrency (config.llm.max_concurrent),
                  config.llm.timeout_s),
      llm_client_ (llm_proxy_, config_.llm),
      registry_ (db_),
      dispatcher_ (),
      forge_coordinator_ (db_, llm_proxy_, registry_,
                          // ForgeCoordinator completion callback —
                          // enqueues MasterDecision to Orchestrator.
                          [this] (forge::ForgeResult result)
                          {
                            send_to_orchestrator (
                              make_forge_complete_event (result));
                          }),
      gateway_ (zmq_ctx_,
                // forward_fn: Gateway → Orchestrator
                [this] (GatewayInbound msg)
                {
                  OrchestratorEvent ev;
                  ev.kind         = OrchestratorEvent::Kind::GatewayInbound;
                  ev.payload_json = std::move (msg.message);
                  // Embed identity into payload so Orchestrator can reply.
                  // (Orchestrator extracts "_identity" field.)
                  // For simplicity we prepend identity into the JSON here.
                  send_to_orchestrator (std::move (ev));
                }),
      orchestrator_ (db_, llm_proxy_, registry_, dispatcher_,
                     forge_coordinator_, config_,
                     // send_to_master
                     [this] (MasterEvent msg) { send_to_master (std::move (msg)); },
                     // send_to_gateway
                     [this] (GatewayEvent msg) { send_to_gateway (std::move (msg)); }),
      master_ (llm_client_, registry_,
               // send_to_orchestrator
               [this] (OrchestratorEvent msg)
               { send_to_orchestrator (std::move (msg)); }),
      periodic_ (db_, dispatcher_, config_,
                 // send_to_orchestrator
                 [this] (OrchestratorEvent msg)
                 { send_to_orchestrator (std::move (msg)); },
                 // send_to_master
                 [this] (MasterEvent msg) { send_to_master (std::move (msg)); },
                 // gateway_push: push raw JSON string to inproc://gateway-out
                 [this] (const std::string &payload)
                 { gateway_push (payload); })
{
}

Central::~Central ()
{
  stop ();
}

// ---------------------------------------------------------------------------
// run — start all actors and block until stop() is called
// ---------------------------------------------------------------------------

void Central::run ()
{
  spdlog::info ("[central] starting");

  // 1. Open database — fail fast.
  if (!db_.open ())
    {
      spdlog::critical ("[central] failed to open database — aborting");
      return;
    }

  // 2. LlmProxy thread pool starts in its constructor (ADR-017).

  // 3. Bind inproc PULL before any thread pushes to gateway-out.
  gateway_.bind_inproc ();

  // 4. Bind gateway_push_ PUSH socket.
  //    Must bind before any push calls (periodic executor fires heartbeat).
  gateway_push_sock_.bind ("inproc://gateway-out-push");
  // Note: Gateway PULL binds to "inproc://gateway-out";
  //       we push to the same endpoint from gateway_push().

  // 5. Init and start Orchestrator.
  orchestrator_.init ();
  orchestrator_.start ();

  // 6. Start Master.
  master_.start ();

  // 7. Init and start PeriodicExecutor.
  periodic_.init ();
  periodic_.start ();

  // 8. Start Gateway last — external interface opens only when ready.
  gateway_.start ();

  spdlog::info ("[central] all actors started");

  // Block until stop() is called.
  while (!stop_requested_)
    std::this_thread::sleep_for (std::chrono::milliseconds (100));

  stop_all ();
}

void Central::stop ()
{
  stop_requested_ = true;
}

// ---------------------------------------------------------------------------
// stop_all — reverse startup order
// ---------------------------------------------------------------------------

void Central::stop_all ()
{
  spdlog::info ("[central] stopping");

  gateway_.stop ();
  periodic_.stop ();
  master_.stop ();
  orchestrator_.stop ();
  forge_coordinator_.stop ();
  // LlmProxy worker threads are joined in its destructor (ADR-017).
  db_.close ();

  spdlog::info ("[central] stopped");
}

// ---------------------------------------------------------------------------
// Routing helpers
// ---------------------------------------------------------------------------

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
  // Push outbound message to inproc://gateway-out.
  // Gateway's PULL socket reads it and forwards to the client.
  if (msg.kind == GatewayEvent::Kind::Outbound)
    gateway_push (msg.outbound.message, msg.outbound.identity);
}

void Central::send_to_periodic (PeriodicControl msg)
{
  periodic_.enqueue (std::move (msg));
}

// ---------------------------------------------------------------------------
// gateway_push — push raw JSON to inproc://gateway-out
// Frames: [identity][payload] for targeted replies,
//         or [payload] for broadcasts (Gateway handles both).
// ---------------------------------------------------------------------------

void Central::gateway_push (const std::string &payload,
                             const std::string &identity)
{
  if (!identity.empty ())
    {
      zmq::message_t id_frame (identity.data (), identity.size ());
      gateway_push_sock_.send (id_frame, zmq::send_flags::sndmore);
    }
  zmq::message_t payload_frame (payload.data (), payload.size ());
  gateway_push_sock_.send (payload_frame, zmq::send_flags::none);
}

// ---------------------------------------------------------------------------
// Helper: build OrchestratorEvent from ForgeResult
// ---------------------------------------------------------------------------

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
