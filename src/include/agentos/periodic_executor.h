#pragma once
/**
 * agentos/periodic_executor.h
 *
 * PeriodicExecutor — single thread owning all time-driven work (ADR-023).
 *
 * Registered tasks fire at their scheduled time and are routed to the
 * correct destination via callbacks injected at construction.
 *
 * Tasks are maintained in a min-heap ordered by next_fire.
 * Control messages (Register/Cancel) arrive via Actor<PeriodicControl>
 * queue and are drained before each heap check.
 *
 * Built-in seeded task: heartbeat (target: Gateway inproc push).
 */

#include "agentos/actor.h"
#include "agentos/config.h"
#include "agentos/database/database.h"
#include "agentos/dispatcher.h"
#include "agentos/types.h"

#include <chrono>
#include <functional>
#include <queue>
#include <string>
#include <vector>

namespace agentos
{

class PeriodicExecutor : public Actor<PeriodicControl>
{
public:
  using SendToOrchestrator = std::function<void(OrchestratorEvent)>;
  using SendToMaster       = std::function<void(MasterEvent)>;
  using GatewayPush        = std::function<void(const std::string&)>;

  PeriodicExecutor (Database           &db,
                    Dispatcher         &dispatcher,
                    const Config       &config,
                    SendToOrchestrator  send_to_orchestrator,
                    SendToMaster        send_to_master,
                    GatewayPush         gateway_push);

  ~PeriodicExecutor () = default;

  PeriodicExecutor (const PeriodicExecutor &)            = delete;
  PeriodicExecutor &operator= (const PeriodicExecutor &) = delete;

  // Called by Central before start() — loads tasks from DB, seeds heartbeat.
  void init ();

private:
  // ---------------------------------------------------------------------------
  // Actor overrides
  // ---------------------------------------------------------------------------

  // PeriodicExecutor overrides loop() to use pop_for() with a short timeout
  // so it can check the heap between message drains.
  void loop () override;

  // Process a single control message (Register or Cancel).
  void on_message (PeriodicControl msg) override;

  // ---------------------------------------------------------------------------
  // Internal task type
  // ---------------------------------------------------------------------------

  struct Task
  {
    std::string id;
    int64_t     next_fire  = 0; // Unix seconds
    int64_t     interval_s = 0; // 0 = one-shot
    std::string target;         // "gateway" | "orchestrator" | "master"
    std::string payload_json;

    // Min-heap: smallest next_fire at top.
    bool operator> (const Task &o) const { return next_fire > o.next_fire; }
  };

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------

  void        register_task (PeriodicControl::Task t);
  void        cancel_task   (const std::string &id);
  void        fire          (const Task &t);
  int64_t     now_s         () const;
  void        persist_task  (const Task &t);
  void        mark_disabled (const std::string &id);

  // Build the heartbeat payload at fire time (reads live DB stats).
  std::string build_heartbeat_payload () const;

  // ---------------------------------------------------------------------------
  // Members
  // ---------------------------------------------------------------------------

  Database           &db_;
  Dispatcher         &dispatcher_;
  const Config       &config_;
  SendToOrchestrator  send_to_orchestrator_;
  SendToMaster        send_to_master_;
  GatewayPush         gateway_push_;

  // Min-heap — accessed only on the PeriodicExecutor thread.
  std::priority_queue<Task,
                      std::vector<Task>,
                      std::greater<Task>> heap_;

  int64_t daemon_start_s_ = 0;
};

} // namespace agentos
