#ifndef AGENTOS_LAYER_B_H
#define AGENTOS_LAYER_B_H

#include <string>
#include <vector>
#include <optional>
#include "agentos/types.h"

namespace agentos {

// Forward declarations
class Registry;
class Database;
class ForgeDatabase;

/**
 * @brief Deterministic Layer B – security and resource decisions.
 *
 * Layer B decisions are never influenced by LLM output.
 * All methods are pure deterministic logic.
 */
class LayerB {
public:
    explicit LayerB(Registry& registry, Database& db);

    // ---- Capability matching (law; cannot be overridden by LLM) ----
    bool capability_allowed(const CapabilityDeclaration& decl,
                            const std::string& job_dir) const;

    // ---- Resource evaluation (reads /proc/meminfo + current cgroup usage) ----
    struct ResourceUsage {
        uint64_t mem_total_kb;
        uint64_t mem_available_kb;
        uint64_t cgroup_mem_usage_kb;
        uint64_t cgroup_mem_limit_kb;
    };
    ResourceUsage evaluate_resources() const;

    // ---- Job state machine progression (deterministic) ----
    bool can_transition(const std::string& current_phase,
                        const std::string& next_phase) const;

    // ---- SandboxProbe validation in the Forge pipeline ----
    bool validate_sandbox_probe(const std::string& job_id,
                                const std::string& code) const;

    // ---- Human escalation threshold enforcement ----
    bool human_escalation_required(const std::string& job_id,
                                   int attempt,
                                   int max_attempts) const;

private:
    Registry& registry_;
    Database& db_;
};

} // namespace agentos

#endif // AGENTOS_LAYER_B_H
