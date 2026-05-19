#pragma once
/**
 * agentos/capability.h
 *
 * ADR-006 Layer 2: Capability Declaration validation.
 */

#include <string>
#include <vector>

#include "agentos/types.h"

namespace agentos {

// Validate a capability declaration against the policy defined in ADR-006.
// Returns true if the declaration is allowed, false if it should be rejected.
// job_dir is the directory of the job (used to check fs_read paths).
bool validate_capability(const CapabilityDeclaration &decl,
                         const std::string &job_dir);

// Determine the sandbox tier based on the declaration.
SandboxTier determine_tier(const CapabilityDeclaration &decl);

} // namespace agentos
