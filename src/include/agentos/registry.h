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

#include <memory>
#include "agentos/types.h"
#include <optional>

namespace agentos
{

  class Registry
  {
  public:
    Registry ();
    explicit Registry (Database &db);
    ~Registry ();

    Registry (const Registry &) = delete;
    Registry &operator= (const Registry &) = delete;

    Registry (Registry &&other) noexcept;
    Registry &operator= (Registry &&other) noexcept;

    // Load static catalog from SQLite database (ADR-007)
    void load_from_db (Database &db);

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

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };

} // namespace agentos
