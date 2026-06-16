/**
 * tests/cred_vault_test.cpp
 *
 * Unit tests for ADR-028 CredVault.
 *
 * Uses an in-memory SQLite database and a temp HOME directory so the
 * SoftwareTPMBackend can write its vault.sealed / tpm.state without
 * affecting the user's real ~/.agentos.
 *
 * Test status — ADR-028 implementation is partial:
 *   GTEST_SKIP() is used to mark tests that depend on CredVault methods
 *   currently stubbed in src/core/cred_vault.cpp (list/rekey/revoke return
 *   without doing real work). Remove the SKIP line as each method is
 *   completed; the assertion that follows is already the regression check
 *   for the finished feature.
 */
#include "agentos/config.h"
#include "agentos/cred_vault.h"
#include "agentos/database.h"
#include "agentos/types.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

using namespace agentos;

namespace
{

std::filesystem::path make_tmp_home ()
{
  auto base = std::filesystem::temp_directory_path ();
  auto now  = std::chrono::steady_clock::now ().time_since_epoch ().count ();
  std::random_device rd;
  auto path = base
              / ("agentos-credtest-" + std::to_string (now) + "-"
                 + std::to_string (rd ()));
  std::filesystem::create_directories (path);
  return path;
}

class CredVaultTest : public ::testing::Test
{
protected:
  void SetUp () override
  {
    home_ = make_tmp_home ();
    saved_home_ = std::getenv ("HOME") ? std::getenv ("HOME") : "";
    setenv ("HOME", home_.c_str (), 1);

    db_ = std::make_unique<Database> (":memory:");
    ASSERT_TRUE (db_->open ());

    Config::Vault cfg{}; // defaults → community tier → SoftwareTPMBackend
    vault_ = std::make_unique<CredVault> (*db_, cfg);
    auto r = vault_->start ();
    ASSERT_TRUE (r.has_value ()) << r.error ();
  }

  void TearDown () override
  {
    if (vault_)
    {
      vault_->stop ();
      vault_.reset ();
    }
    if (db_)
    {
      db_->close ();
      db_.reset ();
    }

    if (!saved_home_.empty ())
      setenv ("HOME", saved_home_.c_str (), 1);
    else
      unsetenv ("HOME");

    std::error_code ec;
    std::filesystem::remove_all (home_, ec);
  }

  std::filesystem::path        home_;
  std::string                  saved_home_;
  std::unique_ptr<Database>    db_;
  std::unique_ptr<CredVault>   vault_;
};

} // namespace

// ---------------------------------------------------------------------------
// Sealed lifecycle
// ---------------------------------------------------------------------------

TEST_F (CredVaultTest, IsSealedAfterStart)
{
  EXPECT_TRUE (vault_->is_sealed ());
}

TEST_F (CredVaultTest, NotSealedAfterClear)
{
  vault_->clear ();
  EXPECT_FALSE (vault_->is_sealed ());
}

// ---------------------------------------------------------------------------
// submit / list / revoke
// ---------------------------------------------------------------------------

TEST_F (CredVaultTest, SubmitReturnsNonEmptyId)
{
  auto r = vault_->submit ("user-1", "deepseek", "sk-token-abc",
                           std::nullopt, std::nullopt);
  ASSERT_TRUE (r.has_value ()) << r.error ();
  EXPECT_FALSE (r->empty ());
}

TEST_F (CredVaultTest, ListAfterSubmitContainsCredential)
{

  ASSERT_TRUE (vault_->submit ("user-1", "deepseek", "sk-token", std::nullopt,
                                std::nullopt)
                 .has_value ());

  auto rows = vault_->list ("user-1");
  ASSERT_EQ (rows.size (), 1u);
  EXPECT_EQ (rows[0].user_id, "user-1");
  EXPECT_EQ (rows[0].provider, "deepseek");
}

TEST_F (CredVaultTest, ListForUnknownUserIsEmpty)
{
  // This test happens to pass against the stub (which always returns empty),
  // and remains the correct behaviour once list() is implemented.
  auto rows = vault_->list ("nobody");
  EXPECT_TRUE (rows.empty ());
}

TEST_F (CredVaultTest, RevokeRemovesCredential)
{

  ASSERT_TRUE (vault_->submit ("user-1", "deepseek", "sk-token", std::nullopt,
                                std::nullopt)
                 .has_value ());
  EXPECT_TRUE (vault_->revoke ("user-1", "deepseek"));

  auto rows = vault_->list ("user-1");
  EXPECT_TRUE (rows.empty ());
}

TEST_F (CredVaultTest, RevokeNonexistentReturnsFalse)
{

  EXPECT_FALSE (vault_->revoke ("ghost", "deepseek"));
}

TEST_F (CredVaultTest, SubmitWithRefreshAndExpiry)
{

  auto r = vault_->submit ("user-1", "shopify", "access-tok",
                           std::optional<std::string>{"refresh-tok"},
                           std::optional<int64_t>{2000000000});
  ASSERT_TRUE (r.has_value ()) << r.error ();

  auto rows = vault_->list ("user-1");
  ASSERT_EQ (rows.size (), 1u);
  EXPECT_EQ (rows[0].provider, "shopify");
  ASSERT_TRUE (rows[0].expires_at.has_value ());
  EXPECT_EQ (*rows[0].expires_at, 2000000000);
}

// ---------------------------------------------------------------------------
// grant / revoke_grant
// ---------------------------------------------------------------------------

TEST_F (CredVaultTest, GrantReturnsNonEmptyId)
{
  auto r = vault_->grant ("worker-x", "deepseek", "admin");
  ASSERT_TRUE (r.has_value ()) << r.error ();
  EXPECT_FALSE (r->empty ());
}

TEST_F (CredVaultTest, RevokeGrantRemovesGrant)
{

  auto g = vault_->grant ("worker-x", "deepseek", "admin");
  ASSERT_TRUE (g.has_value ());

  EXPECT_TRUE (vault_->revoke_grant (*g));
  // Second revoke of the same id should fail.
  EXPECT_FALSE (vault_->revoke_grant (*g));
}

TEST_F (CredVaultTest, RevokeGrantUnknownReturnsFalse)
{

  EXPECT_FALSE (vault_->revoke_grant ("grant-does-not-exist"));
}

// ---------------------------------------------------------------------------
// audit
// ---------------------------------------------------------------------------

TEST_F (CredVaultTest, AuditEmptyInitially)
{
  auto rows
    = vault_->audit (std::nullopt, std::nullopt, std::nullopt, 50);
  EXPECT_TRUE (rows.empty ());
}

TEST_F (CredVaultTest, AuditFilterByUser)
{
  ASSERT_TRUE (vault_->submit ("user-1", "deepseek", "tok", std::nullopt,
                                std::nullopt)
                 .has_value ());
  ASSERT_TRUE (vault_->revoke ("user-1", "deepseek"));

  auto rows
    = vault_->audit (std::optional<std::string>{"user-1"}, std::nullopt,
                     std::nullopt, 50);
  for (const auto &row : rows)
    EXPECT_EQ (row.user_id, "user-1");
}

TEST_F (CredVaultTest, AuditRespectsLimit)
{
  auto rows
    = vault_->audit (std::nullopt, std::nullopt, std::nullopt, 1);
  EXPECT_LE (rows.size (), 1u);
}

// ---------------------------------------------------------------------------
// rekey
// ---------------------------------------------------------------------------

TEST_F (CredVaultTest, RekeyPreservesExistingCredentials)
{

  ASSERT_TRUE (vault_->submit ("user-1", "deepseek", "sk-token", std::nullopt,
                                std::nullopt)
                 .has_value ());

  auto r = vault_->rekey ();
  ASSERT_TRUE (r.has_value ()) << r.error ();

  auto rows = vault_->list ("user-1");
  ASSERT_EQ (rows.size (), 1u);
  EXPECT_EQ (rows[0].provider, "deepseek");
}
