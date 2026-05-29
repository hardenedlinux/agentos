#include "agentos/master.h"
#include "agentos/llm_proxy.h"

#include <spdlog/spdlog.h>

namespace agentos {

Master::Master(Dispatcher&  dispatcher,
               Registry&    registry,
               Verifier&    verifier,
               Scheduler&   scheduler,
               const std::string& db_path,
               int llm_max_concurrent,
               int llm_timeout_s)
    : proxy_(llm_max_concurrent, llm_timeout_s),
      orchestrator_(registry, verifier, scheduler, dispatcher, db_path)
{}

TaskResult Master::submit(const Task& task) {
    spdlog::info("[master] task submitted: {} goal='{}'", task.id, task.goal);
    return orchestrator_.submit(task);
}

} // namespace agentos
