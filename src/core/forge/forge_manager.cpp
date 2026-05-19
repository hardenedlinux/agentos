#include "forge_manager.hpp"
#include "database.hpp"
#include "registry.hpp"
#include "dispatcher.hpp"
#include "orchestrator.hpp"
#include "obs_bus.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <random>
#include <sstream>

namespace agentos {

ForgeManager::ForgeManager(Database& db,
                           Registry& registry,
                           Dispatcher& dispatcher,
                           Orchestrator& orchestrator,
                           ObsBus& obsBus)
    : db_(db)
    , registry_(registry)
    , dispatcher_(dispatcher)
    , orchestrator_(orchestrator)
    , obsBus_(obsBus)
{
    forgeDb_ = std::make_unique<ForgeDatabase>(db_);
}

void ForgeManager::initialize() {
    forgeDb_->create_tables();
    setup_state_machine();
}

void ForgeManager::setup_state_machine() {
    stateMachine_ = std::make_unique<ForgeStateMachine>(
        // DraftCallback
        [this](ForgeJob& job) {
            spdlog::info("[forge] drafting job {}", job.id);
            // TODO: call Code Writer Adviser
            // For now, just set last_code to a placeholder
            job.last_code = "// placeholder code for " + job.method;
            job.last_feedback.clear();
            job.attempt++;
        },
        // ReviewCallback
        [this](ForgeJob& job) {
            spdlog::info("[forge] reviewing job {}", job.id);
            // TODO: call Code Reviewer Adviser
            // For now, assume review passes
        },
        // SandboxProbeCallback
        [this](ForgeJob& job) {
            spdlog::info("[forge] sandbox probing job {}", job.id);
            // TODO: run sandbox probe
            // For now, assume probe passes
        },
        // ApproveCallback
        [this](ForgeJob& job) {
            spdlog::info("[forge] approving job {}", job.id);
            // TODO: register worker in registry
        },
        // PromoteCallback
        [this](ForgeJob& job) {
            spdlog::info("[forge] promoting job {}", job.id);
            // TODO: write to catalog
        },
        // HumanReviewCallback
        [this](ForgeJob& job) {
            spdlog::info("[forge] job {} needs human review", job.id);
            // Pause execution; human will decide via CLI
        }
    );
}

std::string ForgeManager::create_job(const std::string& method,
                                     const std::string& requirement,
                                     int max_attempts) {
    ForgeJob job;
    // Generate UUID
    std::ostringstream oss;
    oss << std::chrono::system_clock::now().time_since_epoch().count();
    job.id = "forge_" + oss.str();
    job.method = method;
    job.requirement = requirement;
    job.attempt = 0;
    job.max_attempts = max_attempts;
    job.phase = "Drafting";
    job.last_code.clear();
    job.last_feedback.clear();
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    job.created_at = now;
    job.updated_at = now;

    forgeDb_->insert_job(job);
    spdlog::info("[forge] created job {} for method {}", job.id, method);
    return job.id;
}

void ForgeManager::process_job(const std::string& job_id) {
    auto opt = forgeDb_->get_job(job_id);
    if (!opt) {
        spdlog::warn("[forge] job {} not found", job_id);
        return;
    }
    ForgeJob job = std::move(*opt);
    stateMachine_->process(job);
    job.updated_at = std::chrono::system_clock::now().time_since_epoch().count();
    forgeDb_->update_job(job);
}

std::optional<ForgeJob> ForgeManager::get_job(const std::string& job_id) {
    return forgeDb_->get_job(job_id);
}

std::vector<ForgeJob> ForgeManager::list_jobs() {
    return forgeDb_->get_all_jobs();
}

std::vector<ForgeJob> ForgeManager::list_human_review_jobs() {
    return forgeDb_->get_jobs_by_phase("HumanReview");
}

void ForgeManager::approve_human_review(const std::string& job_id) {
    auto opt = forgeDb_->get_job(job_id);
    if (!opt) {
        spdlog::warn("[forge] job {} not found for approval", job_id);
        return;
    }
    ForgeJob job = std::move(*opt);
    if (job.phase != "HumanReview") {
        spdlog::warn("[forge] job {} is not in HumanReview phase", job_id);
        return;
    }
    // Move to Approved phase
    job.phase = "Approved";
    job.updated_at = std::chrono::system_clock::now().time_since_epoch().count();
    forgeDb_->update_job(job);
    spdlog::info("[forge] job {} approved by human", job_id);
    // Continue processing
    process_job(job_id);
}

void ForgeManager::reject_human_review(const std::string& job_id, const std::string& reason) {
    auto opt = forgeDb_->get_job(job_id);
    if (!opt) {
        spdlog::warn("[forge] job {} not found for rejection", job_id);
        return;
    }
    ForgeJob job = std::move(*opt);
    if (job.phase != "HumanReview") {
        spdlog::warn("[forge] job {} is not in HumanReview phase", job_id);
        return;
    }
    // Move back to Drafting with feedback
    job.phase = "Drafting";
    job.last_feedback = reason;
    job.updated_at = std::chrono::system_clock::now().time_since_epoch().count();
    forgeDb_->update_job(job);
    spdlog::info("[forge] job {} rejected by human: {}", job_id, reason);
    // Continue processing
    process_job(job_id);
}

} // namespace agentos
