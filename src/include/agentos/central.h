#pragma once
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
 *   db → llm_proxy → registry → forge_coordinator → cred_vault →
 *   orchestrator → master → periodic → gateway
 *
 * Shutdown order (reverse):
 *   gateway → periodic → master → orchestrator → cred_vault →
 *   forge_coordinator → llm_proxy → db
 *
 * Outbound gateway path:
 *   Any actor calls send_to_gateway() → Central::gateway_push() →
 *   Gateway::enqueue_outbound() — thread-safe queue.
 *   Gateway poll thread drains the queue and sends on agentos_sock_.
 *   No inproc socket; no separate dispatch thread.
 */

#include "agentos/config.h"
#include "agentos/cred_vault.h"
#include "agentos/database.h"
#include "agentos/dispatcher.h"
#include "agentos/forge_coordinator.h"
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

  void run ();
  void stop ();

private:
  void send_to_orchestrator (OrchestratorEvent msg);
  void send_to_master       (MasterEvent msg);
  void send_to_gateway      (GatewayEvent msg);
  void send_to_periodic     (PeriodicControl msg);

  // Enqueue payload+identity into Gateway's outbound queue.
  void gateway_push (const std::string &payload,
                     const std::string &identity = {});

  static OrchestratorEvent
  make_forge_complete_event (const forge::ForgeResult &result);

  void stop_all ();

  // ---------------------------------------------------------------------------
  // ZMQ context — must outlive all sockets
  // ---------------------------------------------------------------------------
  zmq::context_t zmq_ctx_{ 1 };

  // ---------------------------------------------------------------------------
  // Shared services
  // ---------------------------------------------------------------------------
  Config    config_;
  Database  db_;
  LlmProxy  llm_proxy_;
  LlmClient llm_client_;
  Registry  registry_;

  // ---------------------------------------------------------------------------
  // Sub-components
  // ---------------------------------------------------------------------------
  Dispatcher              dispatcher_;
  forge::ForgeCoordinator forge_coordinator_;
  CredVault               cred_vault_;

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
