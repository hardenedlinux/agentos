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

#include "agentos/vault_backend.h"
#include "agentos/config.h"
#include "agentos/home_init.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <spdlog/spdlog.h>
#include <unistd.h>

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

    // ---------------------------------------------------------------------------
    // prepare_vault_tpm
    //
    // Everything that MUST happen before TPMLIB_MainInit, in both init() and
    // unseal():
    //
    //   1. chdir into the vault directory.  libtpms writes its NVChip file to
    //      the current working directory via the default NVRAM callbacks.  If
    //      we do not chdir, MainInit on a fresh process (the daemon-restart
    //      case, which goes straight to unseal()) would look for NVChip in the
    //      launch directory, fail to find it, and silently create a fresh empty
    //      TPM in the wrong place.
    //
    //   2. TPMLIB_ChooseTPMVersion(TPM2) — selects the TPM 2.0 personality.
    //      This is process-global library state and MUST be set before MainInit.
    //      On a restart that calls unseal() without init() having run first,
    //      forgetting this leaves the library defaulting to TPM 1.2, which then
    //      cannot load a TPM2 NVChip — historically the source of the SetState
    //      rc=0x43 we saw on the first run after a kill.
    //
    //   3. TPMLIB_SetBufferSize — fixes the command/response buffer size.
    // ---------------------------------------------------------------------------
    std::expected<void, Error> prepare_vault_tpm ()
    {
      std::filesystem::path vdir = agentos_home () / "vault";
      std::error_code ec;
      std::filesystem::create_directories (vdir, ec);
      if (::chdir (vdir.c_str ()) != 0)
        return std::unexpected (Error{"vault chdir failed"});

      TPMLIB_ChooseTPMVersion (TPMLIB_TPM_VERSION_2);
      uint32_t min_sz = 0, max_sz = 0;
      TPMLIB_SetBufferSize (4096, &min_sz, &max_sz);
      return {};
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
  // PERSISTENCE MODEL (ADR-028, community tier):
  //   libtpms is initialised purely through its own file-backed NVRAM (the
  //   "NVChip" file in the vault directory).  NVChip is libtpms's native
  //   persistence mechanism and is designed to survive process restarts and
  //   power loss, so it is the single source of TPM state across runs.
  //
  //   We deliberately do NOT use TPMLIB_GetState / TPMLIB_SetState in addition
  //   to NVChip.  Mixing the two — saving a permanent-state blob with GetState
  //   while ALSO letting libtpms manage NVChip — produces two competing copies
  //   of TPM state that drift apart whenever Terminate rewrites NVChip after
  //   GetState was taken, or whenever a kill -9 truncates NVChip.  On the next
  //   start, SetState then loads a permanent blob that is inconsistent with the
  //   on-disk NVChip and MainInit rejects it (rc=0x43).  Relying on NVChip
  //   alone removes that entire class of inconsistency; if NVChip is ever found
  //   corrupt, try_soft_tpm_init() removes it and MainInit recreates a fresh
  //   one (see the security note below on why a fresh TPM is harmless here).
  //
  // IMPORTANT — vault.sealed is NOT encrypted by the TPM in this
  // implementation:
  //   A complete community-tier implementation would use a libtpms TPM2_Create
  //   command to create a KEYEDHASH object with the vault key as sensitiveData,
  //   then store only the TPM2B_PUBLIC + TPM2B_PRIVATE output blobs (never the
  //   raw key).  Unseal would issue TPM2_Load + TPM2_Unseal to recover the key.
  //
  //   The current code takes the simpler but weaker path: it generates the key
  //   from /dev/urandom and writes it directly (prefixed by a version byte) to
  //   vault.sealed.  The libtpms TPM emulator is initialised so that the call
  //   sequence matches the hardware backend, but it is NOT used for
  //   cryptographic protection of the vault key.  Because of this, the TPM
  //   holds nothing that the key depends on, and recreating a fresh NVChip on
  //   corruption does not endanger the key — the key lives only in vault.sealed.
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
    // tpm_state_path_ is retained only to satisfy the header declaration; it is
    // no longer read or written.  The member can be removed from the header in
    // a follow-up cleanup.
    (void)tpm_state_path_;
  }

  SoftwareTPMBackend::~SoftwareTPMBackend () = default;

  // The vault is considered initialised once the sealed key blob exists.
  // NVChip is libtpms-managed internal state and is intentionally NOT part of
  // this check: a missing or corrupt NVChip is recoverable (MainInit recreates
  // it) and must never cause us to fall back into init(), which would overwrite
  // vault.sealed and destroy the existing key.
  bool SoftwareTPMBackend::is_initialized () const
  {
    return std::filesystem::exists (sealed_path_);
  }

  // Bring up the libtpms TPM2 emulator.  Assumes prepare_vault_tpm() has
  // already run (chdir + ChooseTPMVersion + SetBufferSize).
  //
  //   - Normal path: MainInit reads the existing NVChip and returns 0.
  //   - NVChip corrupt (e.g. truncated by a previous kill -9): MainInit fails;
  //     we remove NVChip and retry, letting MainInit build a fresh one.  This
  //     is safe because the vault key lives in vault.sealed, not in the TPM.
  std::expected<void, Error> SoftwareTPMBackend::try_soft_tpm_init ()
  {
    auto rc = TPMLIB_MainInit ();
    if (rc == 0)
      return {};

    spdlog::warn (
      "[vault] TPMLIB_MainInit failed (rc=0x{:x}), attempting recovery", rc);

    // Drop libtpms internal state and the on-disk NVChip, then retry from a
    // clean slate.  vault.sealed is never touched here.
    TPMLIB_Terminate ();
    {
      std::error_code ec;
      std::filesystem::path vdir = agentos_home () / "vault";
      std::filesystem::remove (vdir / "NVChip", ec);
      spdlog::warn ("[vault] removed NVChip for recovery");
    }

    rc = TPMLIB_MainInit ();
    if (rc != 0)
    {
      spdlog::error (
        "[vault] TPMLIB_MainInit failed after recovery (rc=0x{:x})", rc);
      return std::unexpected (Error{"TPMLIB_MainInit failed"});
    }

    spdlog::info ("[vault] TPM recovery succeeded");
    return {};
  }

  std::expected<void, Error> SoftwareTPMBackend::init ()
  {
    if (auto r = prepare_vault_tpm (); !r)
      return r;

    if (auto res = this->try_soft_tpm_init (); !res)
    {
      spdlog::error ("[vault] TPM init failed: {}", res.error ());
      return std::unexpected (res.error ());
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
        explicit_bzero_local (vault_key.data (), vault_key.size ());
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

    // libtpms persists its own state to NVChip; Terminate flushes it.
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

    if (auto r = prepare_vault_tpm (); !r)
      return std::unexpected (r.error ());

    if (auto res = this->try_soft_tpm_init (); !res)
    {
      spdlog::error ("[vault] TPM init failed during unseal: {}", res.error ());
      return std::unexpected (res.error ());
    }

    // Read the raw vault key from vault.sealed.
    // TODO: replace with TPM2_Load + TPM2_Unseal once the TPM actually seals
    //       the key (see security note above).
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
