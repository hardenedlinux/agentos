#pragma once
/**
 * agentos/capability.h
 *
 * ADR-006 Layer 2: Capability Declaration validation.
 */

#include "agentos/types.h"
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace agentos
{
  static bool namespace_isolation_available ()
  {
    int status = 0;
    pid_t pid = fork ();

    if (pid == 0)
      {
        int ret = unshare (CLONE_NEWNS | CLONE_NEWNET);
        _exit (ret == 0 ? 0 : 1);
      }

    waitpid (pid, &status, 0);
    return WIFEXITED (status) && WEXITSTATUS (status) == 0;
  }

  // Validate a capability declaration against the policy defined in ADR-006.
  // Returns true if the declaration is allowed, false if it should be rejected.
  // job_dir is the directory of the job (used to check fs_read paths).
  bool validate_capability (const CapabilityDeclaration &decl,
                            const std::string &job_dir);

  // Determine the sandbox tier based on the declaration.
  SandboxTier determine_tier (const CapabilityDeclaration &decl);

} // namespace agentos
