#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "agentos/config.h"
#include "agentos/types.h"

namespace agentos
{

  /// Abstract backend for TPM-based key sealing/unsealing.
  class VaultBackend
  {
  public:
    virtual ~VaultBackend () = default;

    /// First‑time initialisation: generate vault_key, seal it, persist state.
    virtual std::expected<void, Error> init () = 0;

    /// On every startup: load persistent state, unseal and return plaintext
    /// vault_key.
    virtual std::expected<std::vector<uint8_t>, Error> unseal () = 0;

    /// Returns true after successful init().
    virtual bool is_initialized () const = 0;
  };

  /// Factory that selects the appropriate backend based on configuration.
  std::unique_ptr<VaultBackend>
  create_vault_backend (const Config::Vault &cfg,
                        const std::filesystem::path &home);

  /// Check whether a hardware TPM device exists.
  bool tpm_available ();

  // ---------------------------------------------------------------------------
  // SoftwareTPMBackend – community tier (libtpms, always compiled)
  // ---------------------------------------------------------------------------
  class SoftwareTPMBackend : public VaultBackend
  {
  public:
    explicit SoftwareTPMBackend (const std::filesystem::path &home);
    ~SoftwareTPMBackend () override;

    std::expected<void, Error> init () override;
    std::expected<std::vector<uint8_t>, Error> unseal () override;
    bool is_initialized () const override;

  private:
    std::expected<void, Error> try_soft_tpm_init ();
    std::filesystem::path tpm_state_path_;
    std::filesystem::path sealed_path_;
    std::filesystem::path nvchip_path_;
  };

  // ---------------------------------------------------------------------------
  // HardwareTPMBackend / HardwareTPMPCRBackend (compiled only with hardware
  // TPM)
  // ---------------------------------------------------------------------------
#ifdef AGENTOS_HARDWARE_TPM

  class HardwareTPMBackend : public VaultBackend
  {
  public:
    explicit HardwareTPMBackend (const std::filesystem::path &home);
    ~HardwareTPMBackend () override;

    std::expected<void, Error> init () override;
    std::expected<std::vector<uint8_t>, Error> unseal () override;
    bool is_initialized () const override;

  private:
    std::filesystem::path sealed_path_;
  };

  class HardwareTPMPCRBackend : public HardwareTPMBackend
  {
  public:
    HardwareTPMPCRBackend (const std::filesystem::path &home,
                           std::vector<int> pcr_selection);
    ~HardwareTPMPCRBackend () override;

    std::expected<void, Error> init () override;
    std::expected<std::vector<uint8_t>, Error> unseal () override;

  private:
    std::vector<int> pcr_selection_;
  };

#endif // AGENTOS_HARDWARE_TPM

} // namespace agentos
