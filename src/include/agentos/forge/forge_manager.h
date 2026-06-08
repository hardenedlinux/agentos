#pragma once

#include "agentos/orchestrator.h"
#include "forge_database.h"
#include "forge_job.h"
#include "forge_state_machine.h"
#include <memory>
#include <string>

namespace agentos
{

  class Database;
  class Registry;
  class Dispatcher;
  class ObsBus;

  class ForgeManager
  {
  public:
    ForgeManager (Database &db, Registry &registry, Dispatcher &dispatcher,
                  Orchestrator &orchestrator, ObsBus &obsBus);

    void initialize ();

    // Create a new forge job
    std::string create_job (const std::string &method,
                            const std::string &requirement,
                            int max_attempts = 3);

    // Process a job through its state machine
    void process_job (const std::string &job_id);

    // Get job info
    std::optional<ForgeJob> get_job (const std::string &job_id);
    std::vector<ForgeJob> list_jobs ();
    std::vector<ForgeJob> list_human_review_jobs ();

    // Human review actions
    void approve_human_review (const std::string &job_id);
    void reject_human_review (const std::string &job_id,
                              const std::string &reason);

  private:
    Database &db_;
    Registry &registry_;
    Dispatcher &dispatcher_;
    Orchestrator &orchestrator_;
    ObsBus &obsBus_;
    std::unique_ptr<ForgeDatabase> forgeDb_;
    std::unique_ptr<ForgeStateMachine> stateMachine_;

    void setup_state_machine ();
  };

} // namespace agentos
