#include "agentos/secure_enclave.h"
#include "agentos/types.h"

#include <cstring>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/mman.h>
#include <unistd.h>
#include <spdlog/spdlog.h>

namespace agentos {
namespace {

// Portable explicit_bzero: volatile pointer prevents dead-store elimination.
void safe_zero(void *s, size_t n) noexcept
{
    volatile auto *p = static_cast<volatile unsigned char *>(s);
    while (n--) *p++ = 0;
}

// HKDF-SHA256 single-step extract+expand for a 32-byte OKM.
// salt  = fixed string "agentos-vault-v1"
// ikm   = vault_key bytes (from the locked page, passed in as raw pointer)
// info  = credential_id (domain separation)
// out   = 32-byte derived key written here
// Returns true on success.
bool hkdf_sha256(const void *ikm, size_t ikm_len,
                 std::string_view info,
                 uint8_t out[32]) noexcept
{
    constexpr std::string_view SALT = "agentos-vault-v1";

    // Step 1: PRK = HMAC-SHA256(salt, ikm)
    uint8_t prk[EVP_MAX_MD_SIZE];
    size_t prk_len = 0;

    EVP_MAC *hmac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if (!hmac) return false;

    OSSL_PARAM params[] = {
        OSSL_PARAM_utf8_string("digest", const_cast<char*>("SHA256"), 0),
        OSSL_PARAM_END
    };

    EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(hmac);
    EVP_MAC_free(hmac);
    if (!ctx) return false;

    bool ok = (EVP_MAC_init(ctx,
                             reinterpret_cast<const unsigned char*>(SALT.data()),
                             SALT.size(), params) == 1
               && EVP_MAC_update(ctx, static_cast<const unsigned char*>(ikm), ikm_len) == 1
               && EVP_MAC_final(ctx, prk, &prk_len, sizeof(prk)) == 1);
    EVP_MAC_CTX_free(ctx);
    if (!ok) { safe_zero(prk, sizeof(prk)); return false; }

    // Step 2: OKM[0:32] = HMAC-SHA256(PRK, info || 0x01)
    EVP_MAC *hmac2 = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if (!hmac2) { safe_zero(prk, sizeof(prk)); return false; }

    OSSL_PARAM params2[] = {
        OSSL_PARAM_utf8_string("digest", const_cast<char*>("SHA256"), 0),
        OSSL_PARAM_END
    };

    EVP_MAC_CTX *ctx2 = EVP_MAC_CTX_new(hmac2);
    EVP_MAC_free(hmac2);
    if (!ctx2) { safe_zero(prk, sizeof(prk)); return false; }

    constexpr uint8_t EXPAND_BYTE = 0x01;
    size_t out_len = 32;
    ok = (EVP_MAC_init(ctx2, prk, prk_len, params2) == 1
          && EVP_MAC_update(ctx2,
                            reinterpret_cast<const unsigned char*>(info.data()),
                            info.size()) == 1
          && EVP_MAC_update(ctx2, &EXPAND_BYTE, 1) == 1
          && EVP_MAC_final(ctx2, out, &out_len, 32) == 1
          && out_len == 32);
    EVP_MAC_CTX_free(ctx2);
    safe_zero(prk, sizeof(prk));
    return ok;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SecureEnclave::SecureEnclave()
{
    page_sz_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    page_ = mmap(nullptr, page_sz_, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page_ == MAP_FAILED)
    {
        spdlog::error("[secure_enclave] mmap failed");
        page_ = nullptr;
        page_sz_ = 0;
        return;
    }
    if (mlock(page_, page_sz_) != 0)
        spdlog::warn("[secure_enclave] mlock failed — key may swap to disk");
    if (madvise(page_, page_sz_, MADV_DONTDUMP) != 0)
        spdlog::warn("[secure_enclave] madvise MADV_DONTDUMP failed");
}

SecureEnclave::~SecureEnclave()
{
    clear();
}

// ---------------------------------------------------------------------------
// seal
// ---------------------------------------------------------------------------

void SecureEnclave::seal(const void *key, size_t len)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!page_ || len > page_sz_)
    {
        spdlog::error("[secure_enclave] seal: invalid size or no page");
        return;
    }
    if (mprotect(page_, page_sz_, PROT_READ | PROT_WRITE) != 0)
    {
        spdlog::error("[secure_enclave] seal: mprotect(RW) failed");
        return;
    }
    std::memcpy(page_, key, len);
    key_len_ = len;
    if (mprotect(page_, page_sz_, PROT_NONE) != 0)
        spdlog::error("[secure_enclave] seal: mprotect(NONE) failed");
}

// ---------------------------------------------------------------------------
// encrypt
// ---------------------------------------------------------------------------

std::expected<CipherBlob, Error>
SecureEnclave::encrypt(std::string_view plaintext, std::string_view info)
{
    if (!page_ || key_len_ == 0)
        return std::unexpected(Error{"enclave not sealed"});

    std::lock_guard<std::mutex> lock(mutex_);

    // Bring page to readable for HKDF derivation.
    if (mprotect(page_, page_sz_, PROT_READ) != 0)
        return std::unexpected(Error{"mprotect read failed"});

    uint8_t derived[32];
    bool ok = hkdf_sha256(page_, key_len_, info, derived);

    // Re-seal immediately — derived key is on the stack.
    mprotect(page_, page_sz_, PROT_NONE);

    if (!ok)
    {
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"HKDF derivation failed"});
    }

    // Generate a fresh 96-bit nonce.
    std::vector<uint8_t> nonce(12);
    if (RAND_bytes(nonce.data(), 12) != 1)
    {
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"RAND_bytes failed"});
    }

    // AES-256-GCM encrypt. Tag (16 bytes) is appended after ciphertext.
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
    {
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"EVP_CIPHER_CTX_new failed"});
    }

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1
        || EVP_EncryptInit_ex(ctx, nullptr, nullptr, derived, nonce.data()) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"encrypt init failed"});
    }

    // Encrypt plaintext.
    std::vector<uint8_t> ciphertext(plaintext.size() + 16); // +16 for tag
    int len1 = 0;
    if (EVP_EncryptUpdate(ctx,
                          ciphertext.data(), &len1,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"encrypt update failed"});
    }

    // Finalise (flushes padding; AES-GCM has none, so len2 == 0 normally).
    int len2 = 0;
    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len1, &len2) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"encrypt final failed"});
    }

    int ct_len = len1 + len2;

    // Retrieve the 16-byte authentication tag and append it.
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16,
                             ciphertext.data() + ct_len) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"get tag failed"});
    }
    EVP_CIPHER_CTX_free(ctx);
    safe_zero(derived, sizeof(derived));

    ciphertext.resize(ct_len + 16);

    CipherBlob blob;
    blob.ciphertext = std::move(ciphertext);
    blob.nonce      = std::move(nonce);
    return blob;
}

// ---------------------------------------------------------------------------
// decrypt
// ---------------------------------------------------------------------------

std::expected<std::string, Error>
SecureEnclave::decrypt(const CipherBlob &blob, std::string_view info)
{
    if (!page_ || key_len_ == 0)
        return std::unexpected(Error{"enclave not sealed"});
    if (blob.ciphertext.size() < 16)
        return std::unexpected(Error{"ciphertext too short"});
    if (blob.nonce.size() != 12)
        return std::unexpected(Error{"invalid nonce length"});

    std::lock_guard<std::mutex> lock(mutex_);

    if (mprotect(page_, page_sz_, PROT_READ) != 0)
        return std::unexpected(Error{"mprotect read failed"});

    uint8_t derived[32];
    bool ok = hkdf_sha256(page_, key_len_, info, derived);

    mprotect(page_, page_sz_, PROT_NONE);

    if (!ok)
    {
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"HKDF derivation failed"});
    }

    // Separate ciphertext body and GCM tag.
    const size_t tag_len    = 16;
    const size_t cipher_len = blob.ciphertext.size() - tag_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
    {
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"EVP_CIPHER_CTX_new failed"});
    }

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(blob.nonce.size()), nullptr) != 1
        || EVP_DecryptInit_ex(ctx, nullptr, nullptr, derived, blob.nonce.data()) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"decrypt init failed"});
    }

    std::string plaintext(cipher_len, '\0');
    int len1 = 0;
    if (EVP_DecryptUpdate(ctx,
                          reinterpret_cast<unsigned char*>(plaintext.data()), &len1,
                          blob.ciphertext.data(),
                          static_cast<int>(cipher_len)) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"decrypt update failed"});
    }

    // Set the expected authentication tag before finalising.
    // The tag is the last 16 bytes of blob.ciphertext.
    // Note: EVP_CTRL_GCM_SET_TAG needs a non-const pointer.
    std::vector<uint8_t> tag(blob.ciphertext.end() - tag_len,
                              blob.ciphertext.end());
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                             static_cast<int>(tag_len),
                             tag.data()) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        safe_zero(derived, sizeof(derived));
        return std::unexpected(Error{"set tag failed"});
    }

    int len2 = 0;
    // EVP_DecryptFinal_ex returns <= 0 if tag verification fails.
    if (EVP_DecryptFinal_ex(ctx,
                            reinterpret_cast<unsigned char*>(plaintext.data()) + len1,
                            &len2) <= 0)
    {
        EVP_CIPHER_CTX_free(ctx);
        safe_zero(derived, sizeof(derived));
        // Zero the partially-decrypted plaintext before returning.
        safe_zero(plaintext.data(), plaintext.size());
        return std::unexpected(Error{"authentication failed"});
    }
    EVP_CIPHER_CTX_free(ctx);
    safe_zero(derived, sizeof(derived));

    plaintext.resize(len1 + len2);
    return plaintext;
}

// ---------------------------------------------------------------------------
// clear
// ---------------------------------------------------------------------------

void SecureEnclave::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (page_)
    {
        if (mprotect(page_, page_sz_, PROT_READ | PROT_WRITE) == 0)
        {
            safe_zero(page_, page_sz_);
            mprotect(page_, page_sz_, PROT_NONE);
        }
        munlock(page_, page_sz_);
        munmap(page_, page_sz_);
        page_    = nullptr;
        page_sz_ = 0;
        key_len_ = 0;
    }
}

} // namespace agentos
