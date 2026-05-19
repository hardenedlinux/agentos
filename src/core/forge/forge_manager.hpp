#ifndef AGENTOS_FORGE_MANAGER_HPP
#define AGENTOS_FORGE_MANAGER_HPP

#include <string>
#include <memory>
#include "forge_job.hpp"
#include "forge_database.hpp"
#include "forge_state_machine.hpp"

namespace agentos {

class Database;
class Registry;
class Dispatcher;
class Orchestrator;
class ObsBus;

class ForgeManager {
public:
    ForgeManager(Database& db,
                 Registry& registry,
                 Dispatcher& dispatcher,
                 Orchestrator& orchestrator,
                 ObsBus& obsBus);

    void initialize();

    // Create a new forge job
    std::string create_job(const std::string& method,
                           const std::string& requirement,
                           int max_attempts = 3);

    // Process a job through its state machine
    void process_job(const std::string& job_id);

    // Get job info
    std::optional<ForgeJob> get_job(const std::string& job_id);
    std::vector<ForgeJob> list_jobs();
    std::vector<ForgeJob> list_human_review_jobs();

    // Human review actions
    void approve_human_review(const std::string& job_id);
    void reject_human_review(const std::string& job_id, const std::string& reason);

private:
    Database& db_;
    Registry& registry_;
    Dispatcher& dispatcher_;
    Orchestrator& orchestrator_;
    ObsBus& obsBus_;
    std::unique_ptr<ForgeDatabase> forgeDb_;
    std::unique_ptr<ForgeStateMachine> stateMachine_;

    void setup_state_machine();
};

} // namespace agentos

#endif // AGENTOS_FORGE_MANAGER_HPP
