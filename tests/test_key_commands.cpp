#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <openssl/sha.h>
#include <sys/random.h>
#include <sstream>
#include <string>
#include <vector>

#include "agentos/access_key.h"
#include "agentos/database.h"
#include <sqlite3.h>

class KeyStoreFixture : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/agentos_key_test_XXXXXX";
        int fd = mkstemp(tmpl);
        ::close(fd);
        db_path_ = tmpl;
        db_ = std::make_unique<agentos::Database>(db_path_);
        ASSERT_TRUE(db_->open());
    }
    void TearDown() override {
        db_->close();
        std::filesystem::remove(db_path_);
    }

    std::string db_path_;
    std::unique_ptr<agentos::Database> db_;
};

namespace {

std::vector<std::uint8_t> random_bytes_test(std::size_t n)
{
    std::vector<std::uint8_t> buf(n);
    auto r = getrandom(buf.data(), buf.size(), 0);
    EXPECT_EQ(static_cast<std::size_t>(r), n);
    return buf;
}

std::string base64url_encode_test(const std::uint8_t* data, std::size_t len)
{
    static const char alph[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    std::uint32_t val = 0;
    int valb = -6;
    for (std::size_t i = 0; i < len; ++i) {
        val = (val << 8) | data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(alph[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
        out.push_back(alph[((val << 8) >> (valb + 8)) & 0x3F]);
    return out;
}

std::string sha256_hex_test(const std::uint8_t* data, std::size_t len)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, len, hash);
    std::ostringstream os;
    for (unsigned char c : hash) {
        os << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<int>(c);
    }
    return os.str();
}

} // anonymous namespace

TEST_F(KeyStoreFixture, GeneratedKeyFormat)
{
    auto raw = random_bytes_test(32);
    std::string b64 = base64url_encode_test(raw.data(), raw.size());
    EXPECT_EQ(b64.size(), 43u);
    for (char c : b64) {
        EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_');
    }
}

TEST_F(KeyStoreFixture, TwoKeysDiffer)
{
    auto a = random_bytes_test(32);
    auto b = random_bytes_test(32);
    EXPECT_NE(base64url_encode_test(a.data(), a.size()),
              base64url_encode_test(b.data(), b.size()));
}

TEST_F(KeyStoreFixture, HashDiffersFromKey)
{
    auto raw  = random_bytes_test(32);
    auto salt = random_bytes_test(16);
    std::vector<std::uint8_t> concat;
    concat.insert(concat.end(), raw.begin(),  raw.end());
    concat.insert(concat.end(), salt.begin(), salt.end());
    std::string hash = sha256_hex_test(concat.data(), concat.size());
    std::string b64  = base64url_encode_test(raw.data(), raw.size());
    EXPECT_NE(hash, b64);
    EXPECT_EQ(hash.size(), 64u);
}

TEST_F(KeyStoreFixture, SaltUniqueness)
{
    auto s1 = random_bytes_test(16);
    auto s2 = random_bytes_test(16);
    EXPECT_NE(base64url_encode_test(s1.data(), s1.size()),
              base64url_encode_test(s2.data(), s2.size()));
}

TEST_F(KeyStoreFixture, InsertAndLoad)
{
    agentos::Database::AccessKey k;
    k.id          = "id-test-1";
    k.key         = "ak_plaintext";
    k.key_hash    = "deadbeef";
    k.key_salt    = "abc";
    k.description = "test key";
    k.role        = "admin";
    k.created_at  = 1000;
    db_->insert_access_key(k);

    auto loaded = db_->load_active_access_keys();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id,   k.id);
    EXPECT_EQ(loaded[0].role, k.role);
}

TEST_F(KeyStoreFixture, RevokeRemovesFromActive)
{
    agentos::Database::AccessKey k;
    k.id         = "rk-id";
    k.key        = "rrr";
    k.key_hash   = "hhh";
    k.role       = "operator";
    k.created_at = 2000;
    db_->insert_access_key(k);

    {
        auto active = db_->load_active_access_keys();
        ASSERT_EQ(active.size(), 1u);
    }

    db_->revoke_access_key("rk-id", "test");
    auto active = db_->load_active_access_keys();
    EXPECT_TRUE(active.empty());
}

TEST_F(KeyStoreFixture, RevokeNonExistent)
{
    db_->revoke_access_key("nonexistent", "test");
    auto active = db_->load_active_access_keys();
    EXPECT_TRUE(active.empty());
}

// ADR-041: keys written before key_digest existed are found after an
// upgrade (digest back-filled at open); bound keys are hash-only.
TEST_F(KeyStoreFixture, DigestBackfillAndHashOnlyBoundKeys)
{
    const std::string raw(64, 'a');
    {
        sqlite3 *h = nullptr;
        ASSERT_EQ(sqlite3_open(db_path_.c_str(), &h), SQLITE_OK);
        const std::string sql =
            "INSERT INTO access_keys (id, key, key_hash, key_salt, role, "
            "created_at) VALUES ('old1', '" + raw + "', 'h', 's', 'admin', 1)";
        ASSERT_EQ(sqlite3_exec(h, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(h);
    }
    db_->close();
    db_ = std::make_unique<agentos::Database>(db_path_);
    ASSERT_TRUE(db_->open());
    auto found = db_->find_active_access_key(raw);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->id, "old1");
    EXPECT_EQ(found->key_digest, agentos::access_key_digest(raw));

    auto bound = agentos::make_access_key("service", "d", std::string("u1"));
    ASSERT_TRUE(bound.has_value());
    EXPECT_TRUE(bound->record.key.empty());
    db_->insert_access_key(bound->record);
    auto b = db_->find_active_access_key(bound->raw);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->user_id.value_or(""), "u1");
    EXPECT_EQ(b->key_hash, agentos::access_key_salted_hash(bound->raw, b->key_salt));

    EXPECT_FALSE(agentos::make_access_key("admin", "d", std::string("u1")).has_value());
    EXPECT_FALSE(agentos::make_access_key("service", "d", std::string("")).has_value());
    EXPECT_EQ(agentos::normalize_access_key("ak_" + raw).value_or(""), raw);
    EXPECT_FALSE(agentos::normalize_access_key("ak_" + raw + "0").has_value());
    EXPECT_FALSE(agentos::normalize_access_key(std::string(64, 'A')).has_value());

    EXPECT_EQ(db_->revoke_user_access_keys("u1", "t"), 1);
    EXPECT_FALSE(db_->find_active_access_key(bound->raw).has_value());
}
