#include "agentos/enforce_layer.h"
#include "agentos/registry.h"
#include "database/database.hpp"
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>

namespace agentos {

EnforceLayer::EnforceLayer(Registry& registry, Database& db)
    : registry_(registry), db_(db)
{}

bool EnforceLayer::capability_allowed(const CapabilityDeclaration& decl,
                                      const std::string& job_dir) const {
    // For now, only allow if network and exec are both false
    return !decl.network && !decl.exec;
}

EnforceLayer::ResourceUsage EnforceLayer::evaluate_resources() const {
    ResourceUsage usage = {0,0,0,0};
    // Read /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") == 0) {
                std::istringstream iss(line.substr(9));
                iss >> usage.mem_total_kb;
            } else if (line.find("MemAvailable:") == 0) {
                std::istringstream iss(line.substr(13));
                iss >> usage.mem_available_kb;
            }
        }
    }
    // Read cgroup memory usage (if available)
    std::ifstream cg_usage("/sys/fs/cgroup/memory/memory.usage_in_bytes");
    if (cg_usage.is_open()) {
        cg_usage >> usage.cgroup_mem_usage_kb;
        usage.cgroup_mem_usage_kb /= 1024;
    }
    std::ifstream cg_limit("/sys/fs/cgroup/memory/memory.limit_in_bytes");
    if (cg_limit.is_open()) {
        cg_limit >> usage.cgroup_mem_limit_kb;
        usage.cgroup_mem_limit_kb /= 1024;
    }
    return usage;
}

bool EnforceLayer::can_transition(const std::string& current_phase,
                                  const std::string& next_phase) const {
    // Define allowed transitions
    if (current_phase == "Drafting" && next_phase == "Reviewing") return true;
    if (current_phase == "Reviewing" && next_phase == "SandboxProbe") return true;
    if (current_phase == "SandboxProbe" && next_phase == "Approved") return true;
    if (current_phase == "Approved" && next_phase == "Promoted") return true;
    if (current_phase == "Promoted" && next_phase == "Done") return true;
    // Also allow human review escalation
    if (current_phase == "Drafting" && next_phase == "HumanReview") return true;
    if (current_phase == "Reviewing" && next_phase == "HumanReview") return true;
    if (current_phase == "SandboxProbe" && next_phase == "HumanReview") return true;
    if (current_phase == "Approved" && next_phase == "HumanReview") return true;
    if (current_phase == "Promoted" && next_phase == "HumanReview") return true;
    // Allow returning from HumanReview to Drafting
    if (current_phase == "HumanReview" && next_phase == "Drafting") return true;
    return false;
}

bool EnforceLayer::validate_sandbox_probe(const std::string& job_id,
                                          const std::string& code) const {
    // For now, accept any non-empty code
    return !code.empty();
}

bool EnforceLayer::human_escalation_required(const std::string& job_id,
                                             int attempt,
                                             int max_attempts) const {
    return attempt > max_attempts;
}

} // namespace agentos
// No changes needed
