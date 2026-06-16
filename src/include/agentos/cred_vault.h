#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "agentos/config.h"
#include "agentos/types.h"
#include "agentos/secure_enclave.h"

namespace agentos
{
class Database;
class VaultBackend;

enum class TokenResult { Ready, Delay, Denied };

struct GetTokenOutcome
{
    TokenResult            status;
    std::optional<int64_t> retry_after;   // valid when status == Delay
};

/// Shared service that manages the credential lifecycle.
/// Owns the SecureEnclave, the VaultBackend, and the background refresh thread.
class CredVault
{
public:
    CredVault(Database &db, const Config::Vault &cfg);
    ~CredVault();

    CredVault(const CredVault &) = delete;
    CredVault &operator=(const CredVault &) = delete;

    /// Must be called after database is open, before the orchestrator starts.
    std::expected<void, Error> start();

    /// Signal the background thread to stop and join it.
    void stop();

    /// Zero the enclave page; the object becomes unusable.
    void clear();

    /// Retrieve a plaintext credential token for a step.
    /// Writes JSON directly to pipe_write_fd and then closes it.
    GetTokenOutcome get_token(
        const std::string &job_id,
        const std::string &step_id,
        const std::string &worker_id,
        std::string_view   provider,
        int                pipe_write_fd);

    // ---- cred.* RPC methods ----

    std::expected<std::string, Error>
    submit(const std::string &user_id,
           const std::string &provider,
           const std::string &plaintext_token,
           const std::optional<std::string> &plaintext_refresh,
           std::optional<int64_t> expires_at);

    bool revoke(const std::string &user_id, const std::string &provider);

    std::expected<std::string, Error>
    grant(const std::string &worker_id,
          const std::string &provider,
          const std::string &granted_by);
    bool revoke_grant(const std::string &grant_id);

    std::vector<CredentialRow> list(const std::string &user_id);

    std::vector<CredentialAuditRow>
    audit(const std::optional<std::string> &user_id,
          const std::optional<std::string> &job_id,
          const std::optional<std::string> &provider,
          int limit = 50);

    std::expected<void, Error> rekey();

    bool is_sealed() const { return enclave_.is_sealed(); }

private:
    void refresh_loop();
    void do_refresh_one(const CredentialRow &row);

    Database                      &db_;
    Config::Vault                  cfg_;
    SecureEnclave                  enclave_;
    std::unique_ptr<VaultBackend>  backend_;

    std::atomic<bool>              running_{false};
    std::thread                    refresh_thread_;
    std::condition_variable        cv_;
    std::mutex                     cv_mutex_;

    std::mutex                     failed_mutex_;
    std::unordered_set<std::string> refresh_failed_;
};

} // namespace agentos
