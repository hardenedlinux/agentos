#include "agentos/master.h"
#include <spdlog/spdlog.h>

namespace agentos {

Master::Master(Dispatcher&  dispatcher,
               Registry&    registry,
               Verifier&    verifier,
               Scheduler&   scheduler)
    : orchestrator_(registry, verifier, scheduler, dispatcher)
{}

TaskResult Master::submit(const Task& task) {
    spdlog::info("[master] task submitted: {} goal='{}'", task.id, task.goal);
    return orchestrator_.submit(task);
}

} // namespace agentos
