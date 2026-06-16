#include "agentos/vault_backend.h"
#include "agentos/config.h"
#include "agentos/home_init.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <spdlog/spdlog.h>

// libtpms — always available
extern "C"
{
#include <libtpms/tpm_error.h>
#include <libtpms/tpm_library.h>
}

// libtss2 — only when hardware TPM support is requested
#ifdef AGENTOS_HARDWARE_TPM
#include <tss2/tss2_esys.h>
#endif

namespace agentos
{
  namespace
  {

    void explicit_bzero_local (void *s, size_t n) noexcept
    {
      volatile auto *p = static_cast<volatile unsigned char *> (s);
      while (n--)
        *p++ = 0;
    }

    std::filesystem::path vault_dir (const std::filesystem::path &home)
    {
      auto d = home / "vault";
      std::filesystem::create_directories (d);
      return d;
    }

    constexpr uint8_t SEAL_FORMAT_VERSION = 0x01;

  } // anonymous namespace

  // ---------------------------------------------------------------------------
  // Factory
  // ---------------------------------------------------------------------------
  std::unique_ptr<VaultBackend>
  create_vault_backend (const Config::Vault &cfg,
                        const std::filesystem::path &home)
  {
#ifdef AGENTOS_HARDWARE_TPM
    if (cfg.tier == "standard" || cfg.tier == "enterprise")
    {
      if (!tpm_available ())
      {
        spdlog::error (
          "[vault] hardware TPM required for tier '{}' but not found",
          cfg.tier);
        return nullptr;
      }
      if (cfg.tier == "enterprise")
        return std::make_unique<HardwareTPMPCRBackend> (home, cfg.pcr_indices);
      return std::make_unique<HardwareTPMBackend> (home);
    }
#else
    (void)cfg; // suppress unused-parameter warning
#endif
    return std::make_unique<SoftwareTPMBackend> (home);
  }

  bool tpm_available ()
  {
    return std::filesystem::exists ("/dev/tpmrm0")
           || std::filesystem::exists ("/dev/tpm0");
  }

  // ===========================================================================
  // SoftwareTPMBackend (community tier, always compiled)
  //
  // SECURITY MODEL (ADR-028, community tier):
  //   This backend calls TPMLIB_MainInit() / TPMLIB_Terminate() to initialise
  //   the libtpms in-process TPM2 emulator, and it reads the TPM's state from
  //   disk on each unseal() call so the TPM context survives process restarts.
  //
  // IMPORTANT — vault.sealed is NOT encrypted by the TPM in this
  // implementation:
  //   A complete community-tier implementation would use a libtpms TPM2_Create
  //   command to create a KEYEDHASH object with the vault key as sensitiveData,
  //   then store only the TPM2B_PUBLIC + TPM2B_PRIVATE output blobs (never the
  //   raw key).  Unseal would reconstruct the TPM context from tpm.state and
  //   issue TPM2_Load + TPM2_Unseal to recover the key.
  //
  //   The current code takes the simpler but weaker path: it generates the key
  //   from /dev/urandom and writes it directly (prefixed by a version byte) to
  //   vault.sealed.  The libtpms TPM emulator is initialised so that the state
  //   file is correctly maintained, but it is NOT used for cryptographic
  //   protection of the vault key.
  //
  //   CONSEQUENCE: any process that can read vault.sealed can recover the vault
  //   key.  The file is created 0600 and inside the agentos home directory, so
  //   the practical protection is OS-level file permissions (same as SSH
  //   private keys stored in ~/.ssh/).  This is acceptable for the community
  //   tier on a single-user development machine but is NOT suitable for
  //   production without the full TPM2_Create / TPM2_Unseal flow.
  //
  //   TODO: replace the raw-key storage with proper TPM2_Create + TPM2_Unseal
  //         using the libtpms command-response API before shipping the
  //         community tier to production.
  // ===========================================================================

  SoftwareTPMBackend::SoftwareTPMBackend (const std::filesystem::path &home)
    : tpm_state_path_ (vault_dir (home) / "tpm.state"),
      sealed_path_ (vault_dir (home) / "vault.sealed")
  {
  }

  SoftwareTPMBackend::~SoftwareTPMBackend () = default;

  bool SoftwareTPMBackend::is_initialized () const
  {
    return std::filesystem::exists (tpm_state_path_)
           && std::filesystem::exists (sealed_path_);
  }

  std::expected<void, Error> SoftwareTPMBackend::init ()
  {
    // Initialise the libtpms TPM emulator.
    TPMLIB_ChooseTPMVersion (TPMLIB_TPM_VERSION_2);
    {
      uint32_t min_sz = 0, max_sz = 0;
      TPMLIB_SetBufferSize (4096, &min_sz, &max_sz);
    }
    if (TPMLIB_MainInit () != 0)
    {
      spdlog::error ("[vault] TPMLIB_MainInit failed");
      return std::unexpected (Error{"TPMLIB_MainInit failed"});
    }

    // Generate a 256-bit vault key from /dev/urandom.
    // NOTE: In the full implementation this would use TPM2_GetRandom via the
    //       libtpms command-response interface instead of direct urandom
    //       access.
    std::vector<uint8_t> vault_key (32);
    {
      std::ifstream ur ("/dev/urandom", std::ios::binary);
      if (!ur)
      {
        TPMLIB_Terminate ();
        return std::unexpected (Error{"cannot read /dev/urandom"});
      }
      ur.read (reinterpret_cast<char *> (vault_key.data ()),
               static_cast<std::streamsize> (vault_key.size ()));
      if (!ur)
      {
        TPMLIB_Terminate ();
        return std::unexpected (Error{"short read from /dev/urandom"});
      }
    }

    // WARNING: This stores the raw vault key.
    // A production implementation must replace this with TPM2_Create output
    // (public + private blobs) and zero the raw key immediately after sealing.
    {
      std::ofstream sealed (sealed_path_, std::ios::binary | std::ios::trunc);
      if (!sealed)
      {
        TPMLIB_Terminate ();
        explicit_bzero_local (vault_key.data (), vault_key.size ());
        return std::unexpected (Error{"cannot write vault.sealed"});
      }
      // Restrict permissions to owner-only (0600).
      // The file was already created by ofstream; set permissions now.
      std::error_code ec;
      std::filesystem::permissions (sealed_path_,
                                    std::filesystem::perms::owner_read
                                      | std::filesystem::perms::owner_write,
                                    std::filesystem::perm_options::replace, ec);
      if (ec)
        spdlog::warn ("[vault] could not chmod vault.sealed: {}",
                      ec.message ());

      sealed.put (static_cast<char> (SEAL_FORMAT_VERSION));
      sealed.write (reinterpret_cast<const char *> (vault_key.data ()),
                    static_cast<std::streamsize> (vault_key.size ()));
    }

    // Persist the libtpms TPM state so unseal() can restore it.
    // TPMLIB_GetState allocates the buffer; we must free() it.
    {
      unsigned char *state_buffer = nullptr;
      uint32_t state_len = 0;
      TPM_RESULT r
        = TPMLIB_GetState (TPMLIB_STATE_PERMANENT, &state_buffer, &state_len);
      if (r != TPM_SUCCESS)
      {
        spdlog::warn ("[vault] TPMLIB_GetState failed (rc=0x{:x}) — tpm.state "
                      "may be incomplete",
                      r);
      }
      std::ofstream st (tpm_state_path_, std::ios::binary | std::ios::trunc);
      if (!st)
      {
        std::free (state_buffer);
        TPMLIB_Terminate ();
        explicit_bzero_local (vault_key.data (), vault_key.size ());
        return std::unexpected (Error{"cannot write tpm.state"});
      }
      if (state_buffer && state_len > 0)
        st.write (reinterpret_cast<const char *> (state_buffer), state_len);
      std::free (state_buffer);
    }

    TPMLIB_Terminate ();
    explicit_bzero_local (vault_key.data (), vault_key.size ());
    spdlog::info (
      "[vault] SoftwareTPMBackend initialised (dev-mode, see ADR-028 TODO)");
    return {};
  }

  std::expected<std::vector<uint8_t>, Error> SoftwareTPMBackend::unseal ()
  {
    if (!is_initialized ())
      return std::unexpected (Error{"vault not initialised"});

    // Restore libtpms state.
    {
      std::ifstream st (tpm_state_path_, std::ios::binary);
      if (!st)
        return std::unexpected (Error{"cannot open tpm.state"});
      std::vector<uint8_t> buf ((std::istreambuf_iterator<char> (st)),
                                std::istreambuf_iterator<char> ());
      if (!buf.empty ())
      {
        TPM_RESULT r = TPMLIB_SetState (TPMLIB_STATE_PERMANENT, buf.data (),
                                        static_cast<uint32_t> (buf.size ()));
        if (r != TPM_SUCCESS)
          spdlog::warn ("[vault] TPMLIB_SetState returned rc=0x{:x}", r);
      }
    }

    if (TPMLIB_MainInit () != 0)
      return std::unexpected (Error{"TPMLIB_MainInit failed"});

    // Read the raw vault key from vault.sealed.
    // TODO: replace with TPM2_Load + TPM2_Unseal using the restored TPM state.
    std::ifstream sealed (sealed_path_, std::ios::binary);
    if (!sealed)
    {
      TPMLIB_Terminate ();
      return std::unexpected (Error{"cannot open vault.sealed"});
    }

    uint8_t ver = 0;
    sealed.read (reinterpret_cast<char *> (&ver), 1);
    if (ver != SEAL_FORMAT_VERSION)
    {
      TPMLIB_Terminate ();
      return std::unexpected (Error{"unknown sealed blob format"});
    }

    std::vector<uint8_t> key (32);
    sealed.read (reinterpret_cast<char *> (key.data ()),
                 static_cast<std::streamsize> (key.size ()));
    if (!sealed)
    {
      TPMLIB_Terminate ();
      return std::unexpected (Error{"truncated vault.sealed"});
    }

    TPMLIB_Terminate ();
    return key;
  }

// ===========================================================================
// HardwareTPMBackend / HardwareTPMPCRBackend
// (compiled only with -DAGENTOS_HARDWARE_TPM)
// ===========================================================================
#ifdef AGENTOS_HARDWARE_TPM

  HardwareTPMBackend::HardwareTPMBackend (const std::filesystem::path &home)
    : sealed_path_ (vault_dir (home) / "vault.sealed")
  {
  }

  HardwareTPMBackend::~HardwareTPMBackend () = default;

  bool HardwareTPMBackend::is_initialized () const
  {
    return std::filesystem::exists (sealed_path_);
  }

  std::expected<void, Error> HardwareTPMBackend::init ()
  {
    // TODO: implement TPM2_CreatePrimary + TPM2_Create + TPM2_EvictControl
    //       via tss2-esys to seal the vault key against the hardware TPM.
    return std::unexpected (Error{"hardware TPM backend not yet implemented"});
  }

  std::expected<std::vector<uint8_t>, Error> HardwareTPMBackend::unseal ()
  {
    return std::unexpected (Error{"hardware TPM backend not yet implemented"});
  }

  HardwareTPMPCRBackend::HardwareTPMPCRBackend (
    const std::filesystem::path &home, std::vector<int> pcr_selection)
    : HardwareTPMBackend (home), pcr_selection_ (std::move (pcr_selection))
  {
  }

  HardwareTPMPCRBackend::~HardwareTPMPCRBackend () = default;

  std::expected<void, Error> HardwareTPMPCRBackend::init ()
  {
    // TODO: implement PCR-bound sealing (TPM2_PolicyPCR + TPM2_Create).
    return std::unexpected (
      Error{"enterprise PCR backend not yet implemented"});
  }

  std::expected<std::vector<uint8_t>, Error> HardwareTPMPCRBackend::unseal ()
  {
    return std::unexpected (
      Error{"enterprise PCR backend not yet implemented"});
  }

#endif // AGENTOS_HARDWARE_TPM

} // namespace agentos
