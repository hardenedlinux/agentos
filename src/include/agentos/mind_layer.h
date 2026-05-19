#ifndef AGENTOS_MIND_LAYER_H
#define AGENTOS_MIND_LAYER_H

#include <string>
#include <vector>
#include <optional>
#include "agentos/types.h"

namespace agentos {

// Forward declarations
class Registry;
class Database;

/**
 * @brief LLM-driven MindLayer – understanding and planning.
 *
 * MindLayer receives raw task requests, calls Planning Adviser to analyse
 * the problem, and determines which capabilities are required.
 * Its output (JSON plan) feeds into EnforceLayer as input.
 */
class MindLayer {
public:
    explicit MindLayer(Registry& registry, Database& db);

    // ---- Analyse a raw task request ----
    // Returns a JSON plan string (to be validated by EnforceLayer)
    std::string analyse_task(const Task& task) const;

    // ---- Determine required capabilities for a plan ----
    std::vector<CapabilityDeclaration> required_capabilities(
        const std::string& plan_json) const;

private:
    Registry& registry_;
    Database& db_;
};

} // namespace agentos

#endif // AGENTOS_MIND_LAYER_H
#ifndef AGENTOS_MIND_LAYER_H
#define AGENTOS_MIND_LAYER_H

#include <string>
#include <vector>
#include <optional>
#include "agentos/types.h"

namespace agentos {

// Forward declarations
class Registry;
class Database;

/**
 * @brief LLM-driven MindLayer – understanding and planning.
 *
 * MindLayer receives raw task requests, calls Planning Adviser to analyse
 * the problem, and determines which capabilities are required.
 * Its output (JSON plan) feeds into EnforceLayer as input.
 */
class MindLayer {
public:
    explicit MindLayer(Registry& registry, Database& db);

    // ---- Analyse a raw task request ----
    // Returns a JSON plan string (to be validated by EnforceLayer)
    std::string analyse_task(const Task& task) const;

    // ---- Determine required capabilities for a plan ----
    std::vector<CapabilityDeclaration> required_capabilities(
        const std::string& plan_json) const;

private:
    Registry& registry_;
    Database& db_;
};

} // namespace agentos

#endif // AGENTOS_MIND_LAYER_H
