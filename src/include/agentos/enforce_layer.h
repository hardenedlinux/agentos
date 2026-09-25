#pragma once
/**
 * agentos/enforce_layer.h
 *
 * EnforceLayer — deterministic security and resource decisions (ADR-009).
 *
 * ADR-009 Layer B: decisions are NEVER influenced by LLM output.
 * All methods are pure deterministic logic.
 *
 * Responsibilities:
 *   - Capability declaration validation (ADR-006 policy table)
 *   - Resource evaluation (/proc/meminfo + cgroup v2)
 *   - Forge pipeline state machine (ADR-008/019)
 *   - SandboxProbe validation
 *   - Human escalation threshold enforcement
 */
#include "agentos/types.h"
#include <cstdint>
#include <string>

namespace agentos
{

  class Registry;
  class Database;

  class EnforceLayer
  {
  public:
    explicit EnforceLayer (Registry &registry, Database &db);

    // ── Capability validation (ADR-006 policy table) ────────────────────────
    //
    // Returns true  → auto-approve (Tier-1 sandbox, network:false + exec:false)
    // Returns false → auto-reject  (network:true OR exec:true OR
    //                                tcp_connect_ports + network:false
    //                                conflict)
    //
    // fs_read paths outside job_dir are not auto-rejected here; they are
    // flagged for human escalation by the caller (Scheduler).
    bool capability_allowed (const CapabilityDeclaration &decl,
                             const std::string &job_dir) const;

    // Returns true if any fs_read/fs_write path in decl escapes job_dir.
    // Used by Scheduler to decide whether human escalation is needed.
    bool requires_path_escalation (const CapabilityDeclaration &decl,
                                   const std::string &job_dir) const;

    // ── Resource evaluation ──────────────────────────────────────────────────
    struct ResourceUsage
    {
      uint64_t mem_total_kb = 0;
      uint64_t mem_available_kb = 0;
      uint64_t cgroup_mem_usage_kb = 0; // 0 if cgroup v2 not available
      uint64_t cgroup_mem_limit_kb = 0; // 0 if unlimited or not available
    };

    // Reads /proc/meminfo and cgroup v2 memory.current / memory.max.
    ResourceUsage evaluate_resources () const;

    // ── Forge pipeline state machine (ADR-008/019) ───────────────────────────
    //
    // Valid transitions (all lowercase, per ADR-019):
    //   drafting   → reviewing
    //   reviewing  → drafting      (reject, retry)
    //   reviewing  → human_review  (max attempts exceeded)
    //   reviewing  → promoted      (accept path via SandboxProbe in Reviewer)
    //   promoted   → (terminal)
    //   rejected   → (terminal)
    //   human_review → drafting    (human approves retry)
    //   human_review → rejected    (human rejects)
    bool can_transition (const std::string &current_phase,
                         const std::string &next_phase) const;

    // ── SandboxProbe validation (ADR-009 Enforce Layer final check) ──────────
    //
    // Deterministic Master-layer re-verification of a Reviewer-accepted worker.
    // Re-applies the ADR-006 policy table regardless of Reviewer verdict:
    //   - network:true  → always rejected
    //   - exec:true     → always rejected
    // Returns empty string on pass, rejection reason on fail.
    std::string
    validate_sandbox_probe (const std::string &job_id,
                            const CapabilityDeclaration &decl) const;

    // ── Human escalation threshold ───────────────────────────────────────────
    bool human_escalation_required (const std::string &job_id, int attempt,
                                    int max_attempts) const;

  private:
    Registry &registry_;
    Database &db_;

    static bool path_escapes_dir (const std::string &path,
                                  const std::string &dir);
  };

} // namespace agentos
