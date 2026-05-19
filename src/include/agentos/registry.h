#pragma once
/**
 * agentos/registry.h
 *
 * Registry — thread-safe store of all connected advisers and workers.
 *
 * Responsibilities:
 *   - Store RegisteredAdviser and RegisteredExecutor on handshake
 *   - Index worker commands by name for O(1) lookup
 *   - Remove entries on disconnect
 *   - Provide command schema to Verifier and Orchestrator
 *
 * The Registry never touches the network. It is pure in-memory state.
 */

#include <memory>
#include "agentos/types.h"
#include <optional>
#include <shared_mutex>

namespace agentos
{

  class Registry
  {
  public:
    Registry () = default;

    Registry (const Registry &) = delete;
    Registry &operator= (const Registry &) = delete;

    Registry (Registry &&other) noexcept;
    Registry &operator= (Registry &&other) noexcept;

    // Registration
    void register_adviser (const RegisteredAdviser &agent);
    void register_worker (const RegisteredExecutor &worker);

    // Deregistration (on disconnect)
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
    mutable std::shared_mutex mutex_;

    std::unordered_map<ClientId, RegisteredAdviser> advisers_;
    std::unordered_map<ClientId, RegisteredExecutor> workers_;

    // Secondary index: command name → worker client id
    std::unordered_map<std::string, ClientId> command_index_;
  };

} // namespace agentos
