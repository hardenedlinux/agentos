/**
 * agentos/src/core/capability/capability.cpp
 *
 * ADR-006 Layer 2 validation.
 */

#include "agentos/capability.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace agentos {

bool validate_capability(const CapabilityDeclaration &decl,
                         const std::string &job_dir) {
    // Rule: network: true → auto-reject
    if (decl.network) {
        return false;
    }

    // Rule: exec: true → always rejected
    if (decl.exec) {
        return false;
    }

    // Rule: fs_read paths must be inside job_dir
    for (const auto &path : decl.fs_read) {
        // Resolve relative to job_dir if not absolute
        std::filesystem::path abs_path =
            std::filesystem::absolute(std::filesystem::path(path));
        std::filesystem::path job_path =
            std::filesystem::absolute(std::filesystem::path(job_dir));

        // Check that the path is within job_dir
        auto rel = std::filesystem::relative(abs_path, job_path);
        if (rel.empty() || rel.native()[0] == '.') {
            // outside job directory → escalate (reject for now)
            return false;
        }
    }

    // fs_write paths are not checked for now (auto-approve if inside job_dir)
    // but we could add similar check.

    return true;
}

SandboxTier determine_tier(const CapabilityDeclaration &decl) {
    // Tier-0: pre-approved catalog workers (no declaration needed)
    // Tier-1: generated workers (declaration present)
    // Since we only call this when a declaration exists, always Tier-1.
    return SandboxTier::Tier1;
}

} // namespace agentos
