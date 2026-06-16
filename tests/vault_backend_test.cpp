/**
 * tests/vault_backend_test.cpp
 *
 * Unit tests for ADR-028 VaultBackend.
 *
 * Covers SoftwareTPMBackend init/unseal lifecycle and on-disk artefacts.
 * HardwareTPMBackend tests are guarded by AGENTOS_HARDWARE_TPM and skipped
 * when /dev/tpm* is not present.
 *
 * NOTE: libtpms uses process-global state. Tests must run sequentially
 * within a single process (the GoogleTest default), and each TEST should
 * use its own temp directory.
 */
#include "agentos/types.h"
#include "agentos/vault_backend.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <string>

using namespace agentos;

namespace
{

  std::filesystem::path make_tmp_home ()
  {
    auto base = std::filesystem::temp_directory_path ();
    auto now = std::chrono::steady_clock::now ().time_since_epoch ().count ();
    std::random_device rd;
    auto path = base
                / ("agentos-vaulttest-" + std::to_string (now) + "-"
                   + std::to_string (rd ()));
    std::filesystem::create_directories (path);
    return path;
  }

  class SoftwareTPMBackendTest : public ::testing::Test
  {
  protected:
    void SetUp () override
    {
      home_ = make_tmp_home ();
    }

    void TearDown () override
    {
      std::error_code ec;
      std::filesystem::remove_all (home_, ec);
    }

    std::filesystem::path home_;
  };

} // namespace

// ---------------------------------------------------------------------------
// SoftwareTPMBackend
// ---------------------------------------------------------------------------

TEST_F (SoftwareTPMBackendTest, FreshBackendIsNotInitialized)
{
  SoftwareTPMBackend backend (home_);
  EXPECT_FALSE (backend.is_initialized ());
}

TEST_F (SoftwareTPMBackendTest, InitSucceedsAndCreatesArtefacts)
{
  SoftwareTPMBackend backend (home_);
  auto r = backend.init ();
  ASSERT_TRUE (r.has_value ()) << r.error ();

  EXPECT_TRUE (backend.is_initialized ());
  EXPECT_TRUE (std::filesystem::exists (home_ / "vault" / "vault.sealed"));
  EXPECT_TRUE (std::filesystem::exists (home_ / "vault" / "tpm.state"));
}

TEST_F (SoftwareTPMBackendTest, SealedFileIsOwnerReadWriteOnly)
{
  SoftwareTPMBackend backend (home_);
  ASSERT_TRUE (backend.init ().has_value ());

  auto sealed = home_ / "vault" / "vault.sealed";
  auto perms = std::filesystem::status (sealed).permissions ();

  using std::filesystem::perms;
  // Must include owner read/write.
  EXPECT_TRUE ((perms & perms::owner_read) != perms::none);
  EXPECT_TRUE ((perms & perms::owner_write) != perms::none);
  // Must NOT have any group or other permissions.
  EXPECT_EQ (perms & perms::group_all, perms::none);
  EXPECT_EQ (perms & perms::others_all, perms::none);
}

TEST_F (SoftwareTPMBackendTest, UnsealReturns32ByteKey)
{
  SoftwareTPMBackend backend (home_);
  ASSERT_TRUE (backend.init ().has_value ());

  auto key = backend.unseal ();
  ASSERT_TRUE (key.has_value ()) << key.error ();
  EXPECT_EQ (key->size (), 32u);
}

TEST_F (SoftwareTPMBackendTest, UnsealIsDeterministicForSameVault)
{
  SoftwareTPMBackend backend (home_);
  ASSERT_TRUE (backend.init ().has_value ());

  auto k1 = backend.unseal ();
  auto k2 = backend.unseal ();
  ASSERT_TRUE (k1.has_value ());
  ASSERT_TRUE (k2.has_value ());
  EXPECT_EQ (*k1, *k2);
}

TEST_F (SoftwareTPMBackendTest, UnsealWithoutInitReturnsError)
{
  SoftwareTPMBackend backend (home_);
  auto r = backend.unseal ();
  EXPECT_FALSE (r.has_value ());
}

TEST_F (SoftwareTPMBackendTest, TruncatedSealedFileReturnsError)
{
  SoftwareTPMBackend backend (home_);
  ASSERT_TRUE (backend.init ().has_value ());

  // Truncate vault.sealed to 1 byte (only the version, no key bytes).
  auto sealed = home_ / "vault" / "vault.sealed";
  {
    std::ofstream out (sealed, std::ios::binary | std::ios::trunc);
    out.put (0x01); // version byte only
  }

  auto r = backend.unseal ();
  EXPECT_FALSE (r.has_value ());
}

TEST_F (SoftwareTPMBackendTest, UnknownSealFormatVersionReturnsError)
{
  SoftwareTPMBackend backend (home_);
  ASSERT_TRUE (backend.init ().has_value ());

  auto sealed = home_ / "vault" / "vault.sealed";

  // Read entire file.
  std::vector<char> contents;
  {
    std::ifstream in (sealed, std::ios::binary);
    contents.assign (std::istreambuf_iterator<char> (in),
                     std::istreambuf_iterator<char> ());
  }
  ASSERT_FALSE (contents.empty ());
  std::cerr << "before: byte[0] = 0x" << std::hex
            << static_cast<int> (static_cast<uint8_t> (contents[0])) << "\n";

  contents[0] = static_cast<char> (0xEE);

  // Write back, scope ensures close+flush.
  {
    std::ofstream out (sealed, std::ios::binary | std::ios::trunc);
    out.write (contents.data (), contents.size ());
  }

  // Verify the change reached disk.
  {
    std::ifstream verify (sealed, std::ios::binary);
    char first = 0;
    verify.read (&first, 1);
    std::cerr << "after:  byte[0] = 0x" << std::hex
              << static_cast<int> (static_cast<uint8_t> (first)) << "\n";
    ASSERT_EQ (static_cast<uint8_t> (first), 0xEE);
  }

  auto r = backend.unseal ();
  if (r.has_value ())
    std::cerr << "unseal returned key of " << r->size () << " bytes\n";
  EXPECT_FALSE (r.has_value ());
}

TEST (SoftwareTPMBackendCrossInstanceTest, TwoVaultsHaveDifferentKeys)
{
  auto home_a = make_tmp_home ();
  auto home_b = make_tmp_home ();

  SoftwareTPMBackend a (home_a);
  SoftwareTPMBackend b (home_b);
  ASSERT_TRUE (a.init ().has_value ());
  ASSERT_TRUE (b.init ().has_value ());

  auto ka = a.unseal ();
  auto kb = b.unseal ();
  ASSERT_TRUE (ka.has_value ());
  ASSERT_TRUE (kb.has_value ());
  EXPECT_NE (*ka, *kb);

  std::error_code ec;
  std::filesystem::remove_all (home_a, ec);
  std::filesystem::remove_all (home_b, ec);
}

// ---------------------------------------------------------------------------
// tpm_available()
// ---------------------------------------------------------------------------

TEST (TpmAvailableTest, ReturnsBoolReflectingDevicePresence)
{
  bool available = tpm_available ();
  bool device_exists = std::filesystem::exists ("/dev/tpmrm0")
                       || std::filesystem::exists ("/dev/tpm0");
  EXPECT_EQ (available, device_exists);
}

// ---------------------------------------------------------------------------
// HardwareTPMBackend — only built with -DAGENTOS_HARDWARE_TPM
// ---------------------------------------------------------------------------
#ifdef AGENTOS_HARDWARE_TPM

class HardwareTPMBackendTest : public ::testing::Test
{
protected:
  void SetUp () override
  {
    if (!tpm_available ())
      GTEST_SKIP () << "no /dev/tpm* on this host, skipping hardware TPM test";
    home_ = make_tmp_home ();
  }

  void TearDown () override
  {
    std::error_code ec;
    std::filesystem::remove_all (home_, ec);
  }

  std::filesystem::path home_;
};

TEST_F (HardwareTPMBackendTest, FreshBackendIsNotInitialized)
{
  HardwareTPMBackend backend (home_);
  EXPECT_FALSE (backend.is_initialized ());
}

TEST_F (HardwareTPMBackendTest, InitReturnsExpectedStubError)
{
  // Current implementation is a stub. When the real TPM2_Create flow lands,
  // update this test to verify success and subsequent unseal.
  HardwareTPMBackend backend (home_);
  auto r = backend.init ();
  EXPECT_FALSE (r.has_value ());
}

TEST_F (HardwareTPMBackendTest, UnsealReturnsExpectedStubError)
{
  HardwareTPMBackend backend (home_);
  auto r = backend.unseal ();
  EXPECT_FALSE (r.has_value ());
}

TEST_F (HardwareTPMBackendTest, PcrBackendStubReturnsErrors)
{
  HardwareTPMPCRBackend backend (home_, std::vector<int>{0, 7});
  EXPECT_FALSE (backend.init ().has_value ());
  EXPECT_FALSE (backend.unseal ().has_value ());
}

#endif // AGENTOS_HARDWARE_TPM
