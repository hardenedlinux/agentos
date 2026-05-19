#include "agentos/orchestrator.h"
#include <spdlog/spdlog.h>

namespace agentos {

Orchestrator::Orchestrator(Dispatcher&  dispatcher,
                            Registry&    registry,
                            Verifier&    verifier,
                            Scheduler&   scheduler)
    : master_(dispatcher, registry, verifier, scheduler)
{}

TaskResult Orchestrator::submit(const Task& task) {
    spdlog::info("[orchestrator] delegating task {} to master", task.id);
    return master_.submit(task);
}

} // namespace agentos
