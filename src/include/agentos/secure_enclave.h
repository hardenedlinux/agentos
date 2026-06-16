#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "agentos/types.h"

namespace agentos
{

/// Ciphertext + nonce pair returned by SecureEnclave::encrypt and consumed
/// by SecureEnclave::decrypt. Stored as BLOBs in the credentials table.
struct CipherBlob
{
    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> nonce;   // 96-bit GCM nonce
};

/// Protect the vault key. The key is stored in a single memory page that is
/// kept PROT_NONE except for the brief moment when the HKDF derivation runs.
/// The derived key lives on the stack and is explicitly zeroed before return.
class SecureEnclave
{
public:
    SecureEnclave();
    ~SecureEnclave();

    SecureEnclave(const SecureEnclave &) = delete;
    SecureEnclave &operator=(const SecureEnclave &) = delete;

    /// Store the vault key. Must be called exactly once after VaultBackend::unseal().
    void seal(const void *key, size_t len);

    /// Encrypt plaintext with a key derived from (vault_key, info) via HKDF‑SHA256.
    std::expected<CipherBlob, Error>
    encrypt(std::string_view plaintext, std::string_view info);

    /// Decrypt a CipherBlob with the same derived key.
    std::expected<std::string, Error>
    decrypt(const CipherBlob &blob, std::string_view info);

    bool is_sealed() const { return key_len_ > 0; }

    /// Zero the page and release system resources. The enclave becomes unusable.
    void clear();

private:
    std::mutex mutex_;
    void      *page_    = nullptr;
    size_t     page_sz_ = 0;
    size_t     key_len_ = 0;
};

}   // namespace agentos
