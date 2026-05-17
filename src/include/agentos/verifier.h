#pragma once
/**
 * agentos/verifier.h
 *
 * Verifier — validates an agent's Plan before the Scheduler executes it.
 *
 * Checks performed against the Registry:
 *   1. Every command in the plan exists in the registry
 *   2. All required args are present
 *   3. Arg types match the command schema
 *   4. No step exceeds command limits (input length, etc.)
 *   5. Dependency graph is a valid DAG (no cycles)
 *   6. Variable references ({{step.field}}) point to real upstream steps
 *
 * The Verifier is stateless — it takes a Plan and a Registry view and
 * returns a VerifyResult. No side effects, trivially testable.
 */

#include "agentos/types.h"
#include "agentos/registry.h"

namespace agentos {

class Verifier {
public:
    explicit Verifier(const Registry& registry);

    // Validate a plan. Returns ok=true only if all checks pass.
    // On failure, errors contains one human-readable entry per violation.
    VerifyResult verify(const Plan& plan) const;

private:
    const Registry& registry_;

    VerifyResult check_commands_exist(const Plan& plan) const;
    VerifyResult check_args(const Plan& plan) const;
    VerifyResult check_dag(const Plan& plan) const;
    VerifyResult check_variable_refs(const Plan& plan) const;
};

} // namespace agentos
