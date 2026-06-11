#pragma once
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
 * agentos/central.h
 *
 * Central — container for all internal actors (ADR-024).
 *
 * Owns shared services and all Actors.
 * Inter-actor communication via callbacks injected at construction time.
 * Actors do not hold a reference to Central.
 *
 * Startup order:
 *   db → llm_proxy → registry → forge_coordinator → orchestrator →
 *   master → periodic → gateway
 *
 * Shutdown order (reverse):
 *   gateway → periodic → master → orchestrator →
 *   forge_coordinator → llm_proxy → db
 */

#include "agentos/config.h"
#include "agentos/database.h"
#include "agentos/dispatcher.h"
#include "agentos/forge/forge_coordinator.h"
#include "agentos/gateway.h"
#include "agentos/llm_client.h"
#include "agentos/llm_proxy.h"
#include "agentos/master.h"
#include "agentos/orchestrator.h"
#include "agentos/periodic_executor.h"
#include "agentos/registry.h"
#include "agentos/types.h"

#include <atomic>
#include <string>
#include <thread>
#include <zmq.hpp>

namespace agentos
{

class Central
{
public:
  explicit Central (const Config &config);
  ~Central ();

  Central (const Central &)            = delete;
  Central &operator= (const Central &) = delete;

  // Start all actors and block until stop() is called (SIGTERM/SIGINT).
  void run ();

  // Signal shutdown. Safe to call from signal handler.
  void stop ();

private:
  // ---------------------------------------------------------------------------
  // Routing — private, only Central calls these
  // ---------------------------------------------------------------------------
  void send_to_orchestrator (OrchestratorEvent msg);
  void send_to_master       (MasterEvent msg);
  void send_to_gateway      (GatewayEvent msg);
  void send_to_periodic     (PeriodicControl msg);

  // Push raw JSON to inproc://gateway-out.
  // identity empty = broadcast; non-empty = targeted reply.
  void gateway_push (const std::string &payload,
                     const std::string &identity = {});

  // Build OrchestratorEvent from ForgeResult (for ForgeCoordinator callback).
  static OrchestratorEvent
  make_forge_complete_event (const forge::ForgeResult &result);

  void stop_all ();

  // ---------------------------------------------------------------------------
  // ZMQ context — must outlive all sockets
  // ---------------------------------------------------------------------------
  zmq::context_t zmq_ctx_{ 1 };

  // PUSH end of inproc://gateway-out — used by gateway_push().
  // Gateway holds the PULL end (bound in gateway_.bind_inproc()).
  zmq::socket_t gateway_push_sock_{ zmq_ctx_, zmq::socket_type::push };

  // ---------------------------------------------------------------------------
  // Shared services — constructed before any Actor
  // ---------------------------------------------------------------------------
  Config   config_;
  Database db_;
  LlmProxy llm_proxy_;
  LlmClient llm_client_;   // thin wrapper around llm_proxy_
  Registry  registry_;

  // ---------------------------------------------------------------------------
  // Sub-components injected into Orchestrator
  // ---------------------------------------------------------------------------
  Dispatcher              dispatcher_;
  forge::ForgeCoordinator forge_coordinator_;

  // ---------------------------------------------------------------------------
  // Actors
  // ---------------------------------------------------------------------------
  Gateway          gateway_;
  Orchestrator     orchestrator_;
  Master           master_;
  PeriodicExecutor periodic_;

  std::atomic<bool> stop_requested_{ false };
};

} // namespace agentos
