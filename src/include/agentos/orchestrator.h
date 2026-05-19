#pragma once
/**
 * agentos/orchestrator.h
 *
 * Orchestrator — entry point for user-submitted tasks.
 * Delegates all decision-making to the Master (per ADR-002).
 *
 * The Orchestrator owns no I/O itself. It delegates:
 *   - Network I/O  → Dispatcher
 *   - State lookup → Registry
 *   - Validation   → Verifier
 *   - Execution    → Scheduler
 *   - Decision     → Master
 */

#include "agentos/types.h"
#include "agentos/dispatcher.h"
#include "agentos/registry.h"
#include "agentos/verifier.h"
#include "agentos/scheduler.h"
#include "agentos/master.h"

namespace agentos {

class Orchestrator {
public:
    Orchestrator(Dispatcher&  dispatcher,
                 Registry&    registry,
                 Verifier&    verifier,
                 Scheduler&   scheduler);

    // Submit a task for execution. Blocking — returns when complete.
    TaskResult submit(const Task& task);

private:
    Master master_;
};

} // namespace agentos
