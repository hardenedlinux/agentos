#pragma once
/**
 * agentos/registry.h
 *
 * Registry — static catalog of agents loaded from SQLite database (ADR-007).
 *
 * Responsibilities:
 *   - Load agents and capabilities from a pre‑populated SQLite database
 *   - Provide read‑only lookup of advisers and workers
 *   - Runtime registration is deprecated (no‑op)
 *
 * The Registry never touches the network. It is pure in-memory state.
 */

#include "agentos/forge_pipeline_job.h" // ADR-019
#include "agentos/types.h"
#include <memory>
#include <optional>

namespace agentos
{
  class Database; // forward declaration

  class Registry
  {
  public:
    explicit Registry (Database &db);
    ~Registry ();

    Registry (const Registry &) = delete;
    Registry &operator= (const Registry &) = delete;

    // Move
    Registry (Registry &&other) noexcept;
    Registry &operator= (Registry &&other) noexcept;

    // Runtime registration is deprecated; these are no-ops.
    void register_adviser (const RegisteredAdviser &agent);
    void register_worker (const RegisteredExecutor &worker);

    // Deregistration is deprecated (static catalog); this is a no-op.
    void remove (const ClientId &id);

    // Lookup

    // Find an adviser that handles the given domain (e.g. "research")
    std::optional<RegisteredAdviser>
    find_adviser (const std::string &domain) const;

    // Find the worker that owns a given command (e.g. "web.search")
    std::optional<RegisteredExecutor>
    find_worker_for_command (const std::string &command) const;

    // Get the full schema for a command — used by Verifier
    std::optional<CommandSchema>
    get_command_schema (const std::string &command) const;

    // List all currently registered commands — used by Orchestrator when
    // building context for adviser planning requests
    std::vector<CommandSchema> all_command_schemas () const;

    // Diagnostics
    size_t adviser_count () const;
    size_t worker_count () const;

    // ADR-019: register a worker after forge pipeline reaches Promoted
    void finalize_worker_promotion (const ForgePipelineJob &job,
                                    const std::string &worker_code,
                                    const std::string &capability_json,
                                    Database &db);

    // Find an adviser by its exact id
    std::optional<RegisteredAdviser>
    find_adviser_by_id (const std::string &id) const;

    // List all registered advisers — used by Master for LLM-driven selection
    std::vector<RegisteredAdviser> all_advisers () const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };

} // namespace agentos
