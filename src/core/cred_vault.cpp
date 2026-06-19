/**
 * Copyright (C) 2026  HardenedLinux community
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * agentos/cred_vault.cpp
 *
 * ADR-028: CredVault — credential lifecycle, SecureEnclave integration,
 * background refresh, and pipe-based token injection.
 */

#include "agentos/cred_vault.h"
#include "agentos/database.h"
#include "agentos/home_init.h"
#include "agentos/secure_enclave.h"
#include "agentos/vault_backend.h"

#include <openssl/crypto.h> // OPENSSL_cleanse
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <chrono>
#include <fstream>

namespace agentos
{

  namespace
  {

    int64_t now_unix ()
    {
      return static_cast<int64_t> (
        std::chrono::duration_cast<std::chrono::seconds> (
          std::chrono::system_clock::now ().time_since_epoch ())
          .count ());
    }

    // Securely zero a std::string using OPENSSL_cleanse (not elided by
    // compiler).
    void secure_zero (std::string &s)
    {
      if (!s.empty ())
        OPENSSL_cleanse (s.data (), s.size ());
    }

    std::string generate_uuid ()
    {
      std::ifstream f ("/proc/sys/kernel/random/uuid");
      if (!f)
      {
        spdlog::error ("[cred_vault] generate_uuid: cannot open "
                       "/proc/sys/kernel/random/uuid");
        return {};
      }
      std::string u;
      std::getline (f, u);
      return u;
    }

  } // anonymous namespace

  // ---------------------------------------------------------------------------
  // Construction / destruction
  // ---------------------------------------------------------------------------

  CredVault::CredVault (Database &db, const Config::Vault &cfg)
    : db_ (db), cfg_ (cfg)
  {
  }

  CredVault::~CredVault ()
  {
    stop ();
  }

  // ---------------------------------------------------------------------------
  // start / stop / clear
  // ---------------------------------------------------------------------------

  std::expected<void, Error> CredVault::start ()
  {
    backend_ = create_vault_backend (cfg_, agentos_home ());
    if (!backend_)
      return std::unexpected (Error{"failed to create vault backend"});

    if (!backend_->is_initialized ())
    {
      auto r = backend_->init ();
      if (!r)
        return r;
    }

    auto key_opt = backend_->unseal ();
    if (!key_opt)
      return std::unexpected (key_opt.error ());

    auto &key = *key_opt;
    enclave_.seal (key.data (), key.size ());
    OPENSSL_cleanse (key.data (), key.size ());

    running_ = true;
    refresh_thread_ = std::thread ([this] { refresh_loop (); });
    spdlog::info ("[cred_vault] started (tier={})", cfg_.tier);
    return {};
  }

  void CredVault::stop ()
  {
    if (running_.exchange (false))
    {
      cv_.notify_all ();
      if (refresh_thread_.joinable ())
        refresh_thread_.join ();
    }
  }

  void CredVault::clear ()
  {
    enclave_.clear ();
  }

  // ---------------------------------------------------------------------------
  // get_token — ADR-028 §"get_token: the external interface"
  // ---------------------------------------------------------------------------

  GetTokenOutcome CredVault::get_token (const std::string &job_id,
                                        const std::string &step_id,
                                        const std::string &worker_id,
                                        std::string_view provider,
                                        int pipe_write_fd)
  {
    // 1. Load job; check phase.
    auto job_opt = db_.load_job (job_id);
    if (!job_opt || job_opt->phase != std::string (db::job_phase::executing))
    {
      spdlog::warn (
        "[cred_vault] get_token: job {} not found or not in executing phase",
        job_id);
      close (pipe_write_fd);
      return {TokenResult::Denied, std::nullopt};
    }
    // Extract user_id from the job record.
    const std::string user_id = job_opt->user_id;

    // 2. Check grant.
    auto grant_opt
      = db_.load_credential_grant (worker_id, std::string (provider));
    if (!grant_opt)
    {
      spdlog::warn (
        "[cred_vault] get_token: no grant for worker={} provider={}", worker_id,
        provider);
      close (pipe_write_fd);
      return {TokenResult::Denied, std::nullopt};
    }

    // 3. Load credential.
    auto cred_opt = db_.load_credential (user_id, std::string (provider));
    if (!cred_opt)
    {
      spdlog::warn (
        "[cred_vault] get_token: no credential for user={} provider={}",
        user_id, provider);
      close (pipe_write_fd);
      return {TokenResult::Denied, std::nullopt};
    }
    const auto &cred = *cred_opt;

    // 4. Expiry logic.
    int64_t ts = now_unix ();
    if (cred.expires_at.has_value ())
    {
      int64_t expires = *cred.expires_at;
      bool expired = (expires <= ts);
      bool near_expiry = (expires <= ts + cfg_.refresh_ahead_s);

      if (expired)
      {
        if (!cred.refresh_ciphertext)
        {
          spdlog::info (
            "[cred_vault] get_token: cred {} expired, no refresh token",
            cred.id);
          close (pipe_write_fd);
          return {TokenResult::Denied, std::nullopt};
        }
        // Expired but has refresh material — check refresh_failed set.
        {
          std::lock_guard lock (failed_mutex_);
          // Whether or not in failed set, signal Delay so refresh thread can
          // act.
          int64_t retry = ts + cfg_.refresh_poll_interval;
          close (pipe_write_fd);
          return {TokenResult::Delay, retry};
        }
      }

      if (near_expiry)
      {
        std::lock_guard lock (failed_mutex_);
        if (refresh_failed_.count (cred.id))
        {
          // Refresh failed — still delay, maybe it recovers.
          close (pipe_write_fd);
          return {TokenResult::Delay, ts + cfg_.refresh_poll_interval};
        }
        // Near expiry but not failed: proceed with current token; refresh
        // thread handles it.
      }
    }

    // 5. Decrypt token.
    CipherBlob blob{cred.ciphertext, cred.nonce};
    auto plain_expected = enclave_.decrypt (blob, cred.id);
    if (!plain_expected)
    {
      spdlog::error ("[cred_vault] get_token: decrypt failed for cred {}",
                     cred.id);
      close (pipe_write_fd);
      return {TokenResult::Denied, std::nullopt};
    }
    std::string plain = std::move (*plain_expected);

    // 6. Write JSON to pipe.
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer (buf);
    writer.StartObject ();
    writer.Key ("provider");
    writer.String (provider.data (),
                   static_cast<rapidjson::SizeType> (provider.size ()));
    writer.Key ("token");
    writer.String (plain.c_str (),
                   static_cast<rapidjson::SizeType> (plain.size ()));
    writer.EndObject ();

    const char *json_data = buf.GetString ();
    size_t json_size = buf.GetSize ();

    ssize_t written = write (pipe_write_fd, json_data, json_size);
    close (pipe_write_fd);

    // Zero plaintext after write.
    secure_zero (plain);

    if (written != static_cast<ssize_t> (json_size))
    {
      spdlog::error ("[cred_vault] get_token: short write to pipe ({}/{})",
                     written, json_size);
      return {TokenResult::Denied, std::nullopt};
    }

    // 7. Audit.
    CredentialAuditRow audit;
    audit.id = generate_uuid ();
    audit.credential_id = cred.id;
    audit.user_id = user_id;
    audit.worker_id = worker_id;
    audit.job_id = job_id;
    audit.step_id = step_id;
    audit.run_id = "0"; // run_id not yet available at this call site
    audit.action = "injected";
    audit.timestamp = now_unix ();
    db_.insert_credential_audit (audit);

    return {TokenResult::Ready, std::nullopt};
  }

  // ---------------------------------------------------------------------------
  // submit — ADR-028 §"Submission flow"
  // ---------------------------------------------------------------------------

  std::expected<std::string, Error>
  CredVault::submit (const std::string &user_id, const std::string &provider,
                     const std::string &plaintext_token,
                     const std::optional<std::string> &plaintext_refresh,
                     std::optional<int64_t> expires_at)
  {
    // Check if a credential already exists for (user_id, provider).
    // If so, we re-use its id so that HKDF info stays consistent.
    std::string cred_id;
    {
      auto existing = db_.load_credential (user_id, provider);
      if (existing)
        cred_id = existing->id;
      else
        cred_id = generate_uuid ();
    }

    if (cred_id.empty ())
      return std::unexpected (Error{"uuid generation failed"});

    // Encrypt token with cred_id as HKDF info (ensures per-credential key).
    auto token_blob = enclave_.encrypt (plaintext_token, cred_id);
    if (!token_blob)
      return std::unexpected (token_blob.error ());

    // Encrypt refresh token using a distinct info suffix to keep keys
    // independent.
    std::optional<CipherBlob> refresh_blob;
    if (plaintext_refresh)
    {
      auto rblob = enclave_.encrypt (*plaintext_refresh, cred_id + ":refresh");
      if (!rblob)
        return std::unexpected (rblob.error ());
      refresh_blob = std::move (*rblob);
    }

    // insert_credential handles both insert and update-in-place paths.
    // Pass cred_id explicitly so the DB uses the same id we used as HKDF info.
    auto id_opt = db_.insert_credential (cred_id, user_id, provider,
                                         *token_blob, refresh_blob, expires_at);
    if (!id_opt)
      return std::unexpected (id_opt.error ());

    // Audit.
    CredentialAuditRow audit;
    audit.id = generate_uuid ();
    audit.credential_id = *id_opt;
    audit.user_id = user_id;
    audit.worker_id = "operator";
    audit.job_id = "0";
    audit.step_id = "0";
    audit.run_id = "0";
    audit.action = "submitted";
    audit.timestamp = now_unix ();
    db_.insert_credential_audit (audit);

    return *id_opt;
  }

  // ---------------------------------------------------------------------------
  // revoke
  // ---------------------------------------------------------------------------

  bool CredVault::revoke (const std::string &user_id,
                          const std::string &provider)
  {
    // ADR-028: soft revocation — set expires_at = now, clear refresh material.
    // For simplicity we delegate to the DB hard-delete; a future iteration
    // can change this to an update-in-place if soft revocation is required.
    bool ok = db_.revoke_credential (user_id, provider);

    CredentialAuditRow audit;
    audit.id = generate_uuid ();
    audit.credential_id = "unknown"; // credential already deleted
    audit.user_id = user_id;
    audit.worker_id = "operator";
    audit.job_id = "0";
    audit.step_id = "0";
    audit.run_id = "0";
    audit.action = "revoked";
    audit.timestamp = now_unix ();
    db_.insert_credential_audit (audit);

    return ok;
  }

  // ---------------------------------------------------------------------------
  // grant / revoke_grant
  // ---------------------------------------------------------------------------

  std::expected<std::string, Error>
  CredVault::grant (const std::string &worker_id, const std::string &provider,
                    const std::string &granted_by)
  {
    return db_.insert_credential_grant (worker_id, provider, granted_by);
  }

  bool CredVault::revoke_grant (const std::string &grant_id)
  {
    return db_.revoke_credential_grant (grant_id);
  }

  // ---------------------------------------------------------------------------
  // list — returns metadata only, never token values
  // ---------------------------------------------------------------------------

  std::vector<CredentialRow> CredVault::list (const std::string &user_id)
  {
    return db_.load_credentials_by_user (user_id);
  }

  // ---------------------------------------------------------------------------
  // audit
  // ---------------------------------------------------------------------------

  std::vector<CredentialAuditRow>
  CredVault::audit (const std::optional<std::string> &user_id,
                    const std::optional<std::string> &job_id,
                    const std::optional<std::string> &provider, int limit)
  {
    return db_.load_credential_audit (user_id, job_id, provider, limit);
  }

  // ---------------------------------------------------------------------------
  // rekey — ADR-028 §"vault rekey"
  // ---------------------------------------------------------------------------

  std::expected<void, Error> CredVault::rekey ()
  {
    if (!enclave_.is_sealed ())
      return std::unexpected (Error{"enclave not sealed"});

    // ---------------------------------------------------------------------------
    // RAII wrappers so plaintext is zeroed on every exit path.
    // ---------------------------------------------------------------------------
    struct PlainEntry
    {
      std::string id;
      std::string token_plain;
      std::optional<std::string> refresh_plain;
      std::optional<int64_t> expires_at;

      PlainEntry () = default;
      PlainEntry (PlainEntry &&) = default;
      PlainEntry &operator= (PlainEntry &&) = default;
      PlainEntry (const PlainEntry &) = delete;
      PlainEntry &operator= (const PlainEntry &) = delete;

      ~PlainEntry ()
      {
        if (!token_plain.empty ())
          OPENSSL_cleanse (token_plain.data (), token_plain.size ());
        if (refresh_plain && !refresh_plain->empty ())
          OPENSSL_cleanse (refresh_plain->data (), refresh_plain->size ());
      }
    };

    struct KeyGuard
    {
      std::vector<uint8_t> key;
      ~KeyGuard ()
      {
        if (!key.empty ())
          OPENSSL_cleanse (key.data (), key.size ());
      }
    };

    // 1. Load all credentials.
    auto all_creds = db_.load_all_credentials ();

    // 2. Decrypt every credential into plaintext.
    // PlainEntry destructor guarantees zeroing on every exit path from here on.
    std::vector<PlainEntry> entries;
    entries.reserve (all_creds.size ());

    for (const auto &cred : all_creds)
    {
      PlainEntry e;
      e.id = cred.id;
      e.expires_at = cred.expires_at;

      CipherBlob ct_blob{cred.ciphertext, cred.nonce};
      auto token_opt = enclave_.decrypt (ct_blob, cred.id);
      if (!token_opt)
      {
        spdlog::error ("[cred_vault] rekey: decrypt token failed for {}: {}",
                       cred.id, token_opt.error ());
        return std::unexpected (Error{"decrypt token failed for " + cred.id
                                      + ": " + token_opt.error ()});
      }
      e.token_plain = std::move (*token_opt);

      if (cred.refresh_ciphertext && cred.refresh_nonce)
      {
        CipherBlob ref_blob{*cred.refresh_ciphertext, *cred.refresh_nonce};
        auto ref_opt = enclave_.decrypt (ref_blob, cred.id + ":refresh");
        if (!ref_opt)
        {
          spdlog::error (
            "[cred_vault] rekey: decrypt refresh failed for {}: {}", cred.id,
            ref_opt.error ());
          return std::unexpected (Error{"decrypt refresh failed for " + cred.id
                                        + ": " + ref_opt.error ()});
        }
        e.refresh_plain = std::move (*ref_opt);
      }

      entries.push_back (std::move (e));
    }

    // 3. Generate new vault key (overwrites sealed blob on disk).
    if (!backend_)
      return std::unexpected (Error{"no vault backend"});

    auto init_res = backend_->init ();
    if (!init_res)
      return std::unexpected (
        Error{"backend init failed: " + init_res.error ()});

    auto new_key_opt = backend_->unseal ();
    if (!new_key_opt)
      return std::unexpected (
        Error{"backend unseal failed: " + new_key_opt.error ()});

    KeyGuard kg;
    kg.key = std::move (*new_key_opt);

    SecureEnclave new_enclave;
    new_enclave.seal (kg.key.data (), kg.key.size ());

    // 4. Re-encrypt all credentials under the new key in a single transaction.
    auto tx_result = db_.with_transaction (
      [&] () -> bool
      {
        for (auto &e : entries)
        {
          auto token_blob = new_enclave.encrypt (e.token_plain, e.id);
          if (!token_blob)
          {
            spdlog::error ("[cred_vault] rekey: encrypt token failed for {}",
                           e.id);
            return false;
          }
          std::optional<CipherBlob> refresh_blob;
          if (e.refresh_plain)
          {
            auto rblob
              = new_enclave.encrypt (*e.refresh_plain, e.id + ":refresh");
            if (!rblob)
            {
              spdlog::error (
                "[cred_vault] rekey: encrypt refresh failed for {}", e.id);
              return false;
            }
            refresh_blob = std::move (*rblob);
          }
          if (!db_.update_credential_full (e.id, *token_blob, refresh_blob,
                                           e.expires_at))
          {
            spdlog::error (
              "[cred_vault] rekey: update_credential_full failed for {}", e.id);
            return false;
          }
        }
        return true;
      });

    // Zero plaintexts now — regardless of outcome.
    entries.clear ();

    if (!tx_result)
    {
      switch (tx_result.error ())
      {
      case Database::DbTxError::CommitFailed:
        // Data may or may not be on disk. Vault key on disk already rotated.
        // Operator must reconcile manually before retrying.
        spdlog::error ("[cred_vault] rekey: COMMIT failed — vault key on disk "
                       "may not match DB; manual reconciliation required");
        break;
      case Database::DbTxError::RollbackFailed:
        // DB connection is in an unknown state. Single-connection singleton
        // means the whole Database instance is suspect until restarted.
        spdlog::error ("[cred_vault] rekey: ROLLBACK failed — DB connection "
                       "suspect; restart required");
        break;
      default:
        spdlog::error ("[cred_vault] rekey: transaction failed — vault key on "
                       "disk may not match DB");
        break;
      }
      return std::unexpected (Error{"rekey transaction failed"});
    }

    // 5. Swap the live enclave to use the new key.
    enclave_.clear ();
    enclave_.seal (kg.key.data (), kg.key.size ());
    // kg destructor zeroes kg.key on scope exit.

    return {};
  }

  // ---------------------------------------------------------------------------
  // refresh_loop — background thread
  // ---------------------------------------------------------------------------

  void CredVault::refresh_loop ()
  {
    spdlog::info ("[cred_vault] refresh thread started");
    while (running_)
    {
      int64_t threshold = now_unix () + cfg_.refresh_ahead_s;
      auto expiring = db_.load_expiring_credentials (threshold);
      for (const auto &cred : expiring)
        do_refresh_one (cred);

      std::unique_lock lock (cv_mutex_);
      cv_.wait_for (lock, std::chrono::seconds (cfg_.refresh_poll_interval),
                    [this] { return !running_.load (); });
    }
    spdlog::info ("[cred_vault] refresh thread stopped");
  }

  // ---------------------------------------------------------------------------
  // do_refresh_one — attempt token refresh for one expiring credential
  // ---------------------------------------------------------------------------

  void CredVault::do_refresh_one (const CredentialRow &cred)
  {
    if (!cred.refresh_ciphertext || !cred.refresh_nonce)
      return;

    // Decrypt refresh token.
    CipherBlob ref_blob{*cred.refresh_ciphertext, *cred.refresh_nonce};
    auto refresh_plain = enclave_.decrypt (ref_blob, cred.id + ":refresh");
    if (!refresh_plain)
    {
      spdlog::error (
        "[cred_vault] do_refresh_one: decrypt refresh failed for cred {}",
        cred.id);
      std::lock_guard lock (failed_mutex_);
      refresh_failed_.insert (cred.id);
      return;
    }

    // TODO: call provider-specific HTTP token refresh endpoint using
    // *refresh_plain. For now we mark as failed to avoid pretending we
    // succeeded.
    secure_zero (*refresh_plain);

    spdlog::warn (
      "[cred_vault] do_refresh_one: HTTP refresh not implemented for cred {}",
      cred.id);
    {
      std::lock_guard lock (failed_mutex_);
      refresh_failed_.insert (cred.id);
    }

    CredentialAuditRow audit;
    audit.id = generate_uuid ();
    audit.credential_id = cred.id;
    audit.user_id = cred.user_id;
    audit.worker_id = "refresher";
    audit.job_id = "0";
    audit.step_id = "0";
    audit.run_id = "0";
    audit.action = "refresh_failed";
    audit.reason = "HTTP refresh not yet implemented";
    audit.timestamp = now_unix ();
    db_.insert_credential_audit (audit);
  }

} // namespace agentos
