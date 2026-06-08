/**
 * agentos/enforce_layer.cpp
 *
 * ADR-006: Capability declaration policy table
 * ADR-008/019: Forge pipeline state machine
 * ADR-009: Enforce Layer — deterministic, never influenced by LLM
 * ADR-011: cgroup v2 on Ubuntu 24.04 (kernel 6.8)
 */
#include "agentos/enforce_layer.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <spdlog/spdlog.h>

namespace agentos
{

  namespace fs = std::filesystem;

  EnforceLayer::EnforceLayer (Registry &registry, Database &db)
    : registry_ (registry), db_ (db)
  {
  }

  // ── Capability validation
  // ────────────────────────────────────────────────────

  bool EnforceLayer::capability_allowed (const CapabilityDeclaration &decl,
                                         const std::string &job_dir) const
  {
    // ADR-006 policy table — deterministic, cannot be overridden by LLM.

    // exec:true → always rejected (ADR-006)
    if (decl.exec)
    {
      spdlog::warn ("[enforce] capability rejected: exec:true");
      return false;
    }

    // network:true → auto-reject (ADR-006)
    if (decl.network)
    {
      spdlog::warn ("[enforce] capability rejected: network:true");
      return false;
    }

    // tcp_connect_ports present + network:false → mutually exclusive (ADR-015)
    if (!decl.tcp_connect_ports.empty () && !decl.network)
    {
      spdlog::warn ("[enforce] capability rejected: tcp_connect_ports set "
                    "but network:false");
      return false;
    }

    // fs_read/fs_write path escalation is signalled separately via
    // requires_path_escalation(); it does not cause an auto-reject here.

    return true;
  }

  bool
  EnforceLayer::requires_path_escalation (const CapabilityDeclaration &decl,
                                          const std::string &job_dir) const
  {
    for (const auto &p : decl.fs_read)
      if (path_escapes_dir (p, job_dir))
        return true;
    for (const auto &p : decl.fs_write)
      if (path_escapes_dir (p, job_dir))
        return true;
    return false;
  }

  bool EnforceLayer::path_escapes_dir (const std::string &path,
                                       const std::string &dir)
  {
    // Lexically normalise both paths and check prefix containment.
    // Template placeholders ({{input_path}}) are treated as escaping.
    if (path.find ("{{") != std::string::npos)
      return true;

    const fs::path p = fs::path (path).lexically_normal ();
    const fs::path d = fs::path (dir).lexically_normal ();
    const auto [dp, pp] = std::mismatch (d.begin (), d.end (), p.begin ());
    return dp != d.end (); // dir prefix not fully consumed → path escapes
  }

  // ── Resource evaluation
  // ──────────────────────────────────────────────────────

  EnforceLayer::ResourceUsage EnforceLayer::evaluate_resources () const
  {
    ResourceUsage usage{};

    // /proc/meminfo
    std::ifstream meminfo ("/proc/meminfo");
    if (meminfo.is_open ())
    {
      std::string line;
      while (std::getline (meminfo, line))
      {
        if (line.compare (0, 9, "MemTotal:") == 0)
        {
          std::istringstream iss (line.substr (9));
          iss >> usage.mem_total_kb;
        }
        else if (line.compare (0, 13, "MemAvailable:") == 0)
        {
          std::istringstream iss (line.substr (13));
          iss >> usage.mem_available_kb;
        }
      }
    }
    else
    {
      spdlog::warn ("[enforce] could not open /proc/meminfo");
    }

    // cgroup v2 (ADR-011: Ubuntu 24.04, kernel 6.8)
    // memory.current: current usage in bytes
    {
      std::ifstream f ("/sys/fs/cgroup/memory.current");
      if (f.is_open ())
      {
        uint64_t bytes = 0;
        f >> bytes;
        usage.cgroup_mem_usage_kb = bytes / 1024;
      }
    }
    // memory.max: "max" means unlimited
    {
      std::ifstream f ("/sys/fs/cgroup/memory.max");
      if (f.is_open ())
      {
        std::string val;
        f >> val;
        if (val != "max")
        {
          try
          {
            usage.cgroup_mem_limit_kb = std::stoull (val) / 1024;
          }
          catch (...)
          {
            usage.cgroup_mem_limit_kb = 0;
          }
        }
      }
    }

    return usage;
  }

  // ── Forge pipeline state machine
  // ─────────────────────────────────────────────

  bool EnforceLayer::can_transition (const std::string &current,
                                     const std::string &next) const
  {
    // ADR-019 state machine (all lowercase).
    // Terminal states: promoted, rejected, human_review (pending decision).
    //
    // drafting   → reviewing               (code written, sent for review)
    // reviewing  → drafting                (reviewer rejected, retry)
    // reviewing  → human_review            (max attempts exceeded)
    // reviewing  → promoted                (reviewer accepted + SandboxProbe
    // pass) human_review → drafting              (human approves retry)
    // human_review → rejected              (human rejects permanently)

    if (current == "drafting" && next == "reviewing")
      return true;
    if (current == "reviewing" && next == "drafting")
      return true;
    if (current == "reviewing" && next == "human_review")
      return true;
    if (current == "reviewing" && next == "promoted")
      return true;
    if (current == "human_review" && next == "drafting")
      return true;
    if (current == "human_review" && next == "rejected")
      return true;

    return false;
  }

  // ── SandboxProbe validation
  // ───────────────────────────────────────────────────

  std::string
  EnforceLayer::validate_sandbox_probe (const std::string &job_id,
                                        const CapabilityDeclaration &decl) const
  {
    // ADR-009: Enforce Layer re-verifies the Reviewer's verdict.
    // Reviewer acceptance does NOT bypass this check.

    if (decl.exec)
    {
      spdlog::error ("[enforce] SandboxProbe: job {} rejected — exec:true",
                     job_id);
      return "exec:true is permanently forbidden";
    }

    if (decl.network)
    {
      spdlog::error ("[enforce] SandboxProbe: job {} rejected — network:true",
                     job_id);
      return "network:true requires explicit human approval";
    }

    if (!decl.tcp_connect_ports.empty () && !decl.network)
    {
      spdlog::error ("[enforce] SandboxProbe: job {} rejected — "
                     "tcp_connect_ports conflicts with network:false",
                     job_id);
      return "tcp_connect_ports and network:false are mutually exclusive";
    }

    spdlog::info ("[enforce] SandboxProbe: job {} passed", job_id);
    return "";
  }

  // ── Human escalation
  // ─────────────────────────────────────────────────────────

  bool EnforceLayer::human_escalation_required (const std::string & /*job_id*/,
                                                int attempt,
                                                int max_attempts) const
  {
    return attempt > max_attempts;
  }

} // namespace agentos
