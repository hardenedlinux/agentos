/**
 * agentos/src/core/capability/capability.cpp
 *
 * ADR-006 Layer 2: Capability Declaration Validation (Enforce Layer)
 * ADR-009: All logic here is deterministic; never influenced by LLM output
 * ADR-015: Worker permission granularity; tcp_connect_ports
 * ADR-019: Template placeholder pre-condition; exec: true permanent reject
 *
 * Policy table (ADR-006 + ADR-015):
 *
 *   exec: true                                     → Reject (always,
 * unconditional) network: true                                  → Reject
 * (auto-reject per ADR-006) network: false AND tcp_connect_ports non-empty →
 * Reject (contradictory declaration) fs_read / fs_write outside worker_dir →
 * Escalate (human approval required) network: false AND exec: false AND all
 * paths within worker_dir                            → Approve
 *
 * worker_dir is the registered worker's static directory:
 *   ~/.agentos/workers/<worker-id>/
 * This directory is read-only and established at registration time (ADR-016).
 * Capability validation always uses this directory as the path boundary,
 * regardless of whether the worker is Tier-0 or Tier-1 (ADR-006).
 *
 * Caller contract (returns Error, not a policy verdict):
 *   - worker_dir must not be empty
 *   - no path in fs_read / fs_write may contain unsubstituted "{{...}}"
 *     placeholders; caller must expand these before calling (ADR-019)
 *
 * SandboxTier (ADR-006, ADR-015):
 *   Tier-0: pre-approved catalog workers (approved_by != "forge")
 *           cgroup + Landlock + libcap drop; mount namespace and seccomp
 * omitted Tier-1: Forge-generated workers (approved_by == "forge") full stack:
 * cgroup + mount namespace + CLONE_NEWNET (conditional) + Landlock + seccomp +
 * libcap drop
 *
 * NetworkMode (ADR-015):
 *   Isolated:       network: false, tcp_connect_ports empty
 *                   → CLONE_NEWNET applied; no TCP Landlock rules
 *   PortRestricted: tcp_connect_ports non-empty
 *                   → CLONE_NEWNET omitted; Landlock TCP connect rules applied
 *                      for declared ports only; no bind rules (workers don't
 * listen)
 */

#include "agentos/capability.h"

#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace agentos
{

  namespace fs = std::filesystem;

  bool namespace_isolation_available ()
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

  // -------------------------------------------------------
  // Internal helpers
  // -------------------------------------------------------

  static bool has_template_placeholder (const std::string &path)
  {
    return path.find ("{{") != std::string::npos;
  }

  /**
   * Resolve `path` to an absolute, lexically-normalised form.
   * Relative paths are resolved relative to `base`.
   *
   * lexically_normal() + component prefix check is used instead of
   * fs::relative() because relative() resolves symlinks in ways that
   * can be exploited to escape the intended root. The lexical check is
   * the policy gate; Landlock is the hard kernel enforcement behind it.
   */
  static fs::path resolve (const std::string &path, const fs::path &base)
  {
    fs::path p (path);
    if (p.is_relative ())
      p = base / p;
    return p.lexically_normal ();
  }

  /**
   * Return true if `candidate` is equal to or a subdirectory of `root`.
   * Both paths must already be lexically normalised.
   */
  static bool is_within (const fs::path &candidate, const fs::path &root)
  {
    auto [root_end, cand_end] = std::mismatch (
      root.begin (), root.end (), candidate.begin (), candidate.end ());
    return root_end == root.end ();
  }

  /**
   * Check a list of paths against worker_dir.
   * Returns an Escalate result if any path falls outside worker_dir;
   * returns std::nullopt if all paths are within bounds.
   */
  static std::optional<CapabilityResult>
  check_paths (const std::vector<std::string> &paths, const char *field,
               const fs::path &worker_dir)
  {
    for (const auto &raw : paths)
    {
      const fs::path norm = resolve (raw, worker_dir);
      if (!is_within (norm, worker_dir))
      {
        return CapabilityResult{
          .verdict = CapabilityVerdict::Escalate,
          .reason = std::format (
            "{} path '{}' (resolved: '{}') is outside worker directory '{}'; "
            "requires human approval (ADR-006, ADR-015)",
            field, raw, norm.string (), worker_dir.string ())};
      }
    }
    return std::nullopt;
  }

  // -------------------------------------------------------
  // Public API
  // -------------------------------------------------------

  /**
   * Validate a capability declaration against the Enforce Layer policy table.
   *
   * Called before every worker spawn, for both Tier-0 and Tier-1 workers.
   * No worker is trusted unconditionally; every execution is checked (ADR-009).
   *
   * @param decl        Capability declaration from the worker manifest.
   * @param worker_dir  Absolute path to the worker's static registered
   * directory
   *                    (~/.agentos/workers/<worker-id>/). Must be non-empty.
   *
   * Returns CapabilityResult on policy decisions (Approve / Reject / Escalate).
   * Returns std::unexpected<Error> on caller contract violations only.
   */
  std::expected<CapabilityResult, Error>
  validate_capability (const CapabilityDeclaration &decl,
                       const fs::path &worker_dir)
  {
    // -----------------------------------------------------
    // Caller contract checks → Error (not a policy verdict)
    // -----------------------------------------------------

    if (worker_dir.empty ())
      return std::unexpected<Error> ("worker_dir must not be empty");

    for (const auto &p : decl.fs_read)
    {
      if (has_template_placeholder (p))
        return std::unexpected<Error> (std::format (
          "fs_read path '{}' contains unsubstituted template placeholder; "
          "caller must expand {{{{...}}}} before calling validate_capability "
          "(ADR-019)",
          p));
    }

    for (const auto &p : decl.fs_write)
    {
      if (has_template_placeholder (p))
        return std::unexpected<Error> (std::format (
          "fs_write path '{}' contains unsubstituted template placeholder; "
          "caller must expand {{{{...}}}} before calling validate_capability "
          "(ADR-019)",
          p));
    }

    // -----------------------------------------------------
    // Hard rejects — ADR-006 + ADR-019, unconditional
    // -----------------------------------------------------

    // exec: true is permanently rejected regardless of any other field or any
    // Reviewer verdict (ADR-006, ADR-019 final SandboxProbe).
    if (decl.exec)
      return CapabilityResult{
        .verdict = CapabilityVerdict::Reject,
        .reason
        = "exec: true is permanently rejected by policy (ADR-006, ADR-019)"};

    // network: true is auto-rejected (ADR-006).
    if (decl.network)
      return CapabilityResult{
        .verdict = CapabilityVerdict::Reject,
        .reason = "network: true is auto-rejected by policy (ADR-006)"};

    // -----------------------------------------------------
    // Contradictory network declaration (ADR-015)
    //
    // network: false  → CLONE_NEWNET applied, no TCP Landlock rules.
    // tcp_connect_ports non-empty → CLONE_NEWNET omitted, Landlock TCP rules.
    // Both cannot be declared simultaneously; reject rather than guess intent.
    // -----------------------------------------------------

    if (!decl.network && !decl.tcp_connect_ports.empty ())
      return CapabilityResult{
        .verdict = CapabilityVerdict::Reject,
        .reason
        = "contradictory network declaration: network: false but "
          "tcp_connect_ports is non-empty; use network: false with no "
          "ports for full isolation, or remove network: false and declare "
          "ports for restricted access (ADR-015)"};

    // -----------------------------------------------------
    // Path bounds check — outside worker_dir → Escalate (ADR-006, ADR-015)
    //
    // fs_read and fs_write are checked symmetrically.
    // -----------------------------------------------------

    const fs::path norm_worker = worker_dir.lexically_normal ();

    if (auto r = check_paths (decl.fs_read, "fs_read", norm_worker); r)
      return *r;
    if (auto r = check_paths (decl.fs_write, "fs_write", norm_worker); r)
      return *r;

    // -----------------------------------------------------
    // All checks passed → Approve
    // -----------------------------------------------------

    return CapabilityResult{.verdict = CapabilityVerdict::Approve,
                            .reason = "capability declaration is clean; "
                                      "worker approved for sandbox execution"};
  }

  /**
   * Determine the sandbox tier for a worker (ADR-006, ADR-015).
   *
   * Tier-0: pre-approved catalog workers (agents.approved_by != "forge")
   *         cgroup + Landlock + libcap drop; mount namespace and seccomp
   * omitted.
   *
   * Tier-1: Forge-generated workers (agents.approved_by == "forge")
   *         Full stack: cgroup + mount namespace + CLONE_NEWNET (conditional on
   *         NetworkMode) + Landlock + seccomp + libcap drop.
   *
   * @param forge_generated  True if agents.approved_by == "forge".
   */
  SandboxTier determine_tier (bool forge_generated) noexcept
  {
    return forge_generated ? SandboxTier::Tier1 : SandboxTier::Tier0;
  }

  /**
   * Determine the network isolation mode for a worker (ADR-015).
   *
   * Must be called only after validate_capability() returns Approve; the
   * contradictory-declaration check in validate_capability() guarantees
   * exactly one branch below applies.
   *
   * @param decl  A declaration that has already passed validate_capability().
   */
  NetworkMode
  determine_network_mode (const CapabilityDeclaration &decl) noexcept
  {
    if (!decl.tcp_connect_ports.empty ())
      return NetworkMode::PortRestricted;
    return NetworkMode::Isolated;
  }

} // namespace agentos
