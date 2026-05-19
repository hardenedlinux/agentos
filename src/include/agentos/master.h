#pragma once
/**
 * agentos/master.h
 *
 * Master — the sole decision-maker and resource arbiter per ADR-002.
 *
 * Responsibilities:
 *   - Own the internal subsystems: Dispatcher, Registry, Verifier,
 *     Scheduler, Orchestrator
 *   - Delegate task lifecycle to Orchestrator
 *
 * The Master does not own I/O; it delegates to its subsystems.
 */

#include "agentos/types.h"
#include "agentos/dispatcher.h"
#include "agentos/registry.h"
#include "agentos/verifier.h"
#include "agentos/scheduler.h"
#include "agentos/orchestrator.h"
#include <string>

namespace agentos {

class Master {
public:
    Master(Dispatcher&  dispatcher,
           Registry&    registry,
           Verifier&    verifier,
           Scheduler&   scheduler,
           const std::string& db_path = "");

    // Submit a task for execution. Blocking — returns when complete.
    TaskResult submit(const Task& task);

private:
    Orchestrator orchestrator_;
};

} // namespace agentos
