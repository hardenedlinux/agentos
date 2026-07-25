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

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <rapidjson/document.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "agentos/database.h"
#include "agentos/user_facts.h"

using namespace agentos;

namespace {

// ---------------------------------------------------------------------------
// Raw-sqlite row count helper (parallels test_registry.cpp's `count_rows`).
// Always opens a *separate* connection, never the Database object under test.
// ---------------------------------------------------------------------------
static int rawCountRows(const std::string &dbPath,
                        const std::string &sql,
                        const std::vector<std::string> &binds)
{
    sqlite3 *db = nullptr;
    EXPECT_EQ(sqlite3_open(dbPath.c_str(), &db), SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr),
              SQLITE_OK);

    for (size_t i = 0; i < binds.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1),
                          binds[i].c_str(), -1, SQLITE_TRANSIENT);
    }

    int count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}

// Convenience overload that queries user_fact_events counts.
static int countEventsFor(const std::string &dbPath,
                          const std::string &user,
                          const std::string &factType,
                          const std::string &factKey)
{
    const std::string sql =
        "SELECT COUNT(*) FROM user_fact_events "
        "WHERE user_id=? AND fact_type=? AND fact_key=?";
    return rawCountRows(dbPath, sql, {user, factType, factKey});
}

// Convenience overload that queries user_facts counts.
static int countFactsFor(const std::string &dbPath,
                         const std::string &user,
                         const std::string &factType,
                         const std::string &factKey)
{
    const std::string sql =
        "SELECT COUNT(*) FROM user_facts "
        "WHERE user_id=? AND fact_type=? AND fact_key=?";
    return rawCountRows(dbPath, sql, {user, factType, factKey});
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class UserFactsTest : public ::testing::Test
{
protected:
    std::string db_path_;
    Database *db_ = nullptr;
    std::string agentos_home_dir_;
    bool had_agentos_home_ = false;
    std::string prev_agentos_home_;
    spdlog::level::level_enum prev_log_level_ = spdlog::level::info;

    void SetUp() override
    {
        char tmp[] = "/tmp/agentos_user_facts_test_XXXXXX";
        int fd = mkstemp(tmp);
        ASSERT_NE(fd, -1) << "mkstemp failed";
        close(fd);
        db_path_ = tmp;

        char home_tmp[] = "/tmp/agentos_user_facts_test_home_XXXXXX";
        char *home_dir = mkdtemp(home_tmp);
        ASSERT_NE(home_dir, nullptr) << "mkdtemp failed";
        agentos_home_dir_ = home_dir;

        if (const char *existing = std::getenv("AGENTOS_HOME")) {
            had_agentos_home_ = true;
            prev_agentos_home_ = existing;
        }
        setenv("AGENTOS_HOME", agentos_home_dir_.c_str(), 1);

        prev_log_level_ = spdlog::get_level();
        spdlog::set_level(spdlog::level::off);
    }

    void TearDown() override
    {
        if (db_) {
            db_->close();
            delete db_;
        }
        std::remove(db_path_.c_str());

        std::error_code ec;
        std::filesystem::remove_all(agentos_home_dir_, ec);

        if (had_agentos_home_)
            setenv("AGENTOS_HOME", prev_agentos_home_.c_str(), 1);
        else
            unsetenv("AGENTOS_HOME");

        spdlog::set_level(prev_log_level_);
    }

    Database &open_db()
    {
        db_ = new Database(db_path_);
        EXPECT_TRUE(db_->open());
        return *db_;
    }

    static int64_t nowUnix() { return static_cast<int64_t>(std::time(nullptr)); }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(UserFactsTest, LookupFactTypeKnown)
{
    struct {
        const char *name;
        bool decayed;
    } cases[] = {
        {"category_interest", true},
        {"card_reaction",     false},
        {"risk_preference",   true},
        {"market_region",     true},
    };

    for (auto &c : cases) {
        auto fi = lookup_fact_type(c.name);
        ASSERT_TRUE(fi.has_value());
        EXPECT_STREQ(fi->name, c.name);
        EXPECT_EQ(fi->decayed, c.decayed);
    }
}

TEST_F(UserFactsTest, LookupFactTypeUnknown)
{
    EXPECT_FALSE(lookup_fact_type("no_such_type").has_value());
}

TEST_F(UserFactsTest, RecordNonDecayed)
{
    Database &db = open_db();
    const std::string user = "u1";
    const std::string ft   = "card_reaction"; // non-decayed
    const std::string fk   = "k1";

    Database::UserFactEvent ev;
    ev.user_id    = user;
    ev.fact_type  = ft;
    ev.fact_key   = fk;
    ev.payload    = R"({"value": "abc"})";
    ev.source     = "test";
    ev.created_at = nowUnix();

    double dummy = -1.0;
    auto noop = [](std::optional<double>, double) -> double { return 0.0; };

    ASSERT_TRUE(db.record_user_fact(ev, false, 0.0, noop, "src", dummy));

    // Verify event row was written.
    EXPECT_EQ(countEventsFor(db_path_, user, ft, fk), 1);

    // No user_facts row for non-decayed type.
    EXPECT_EQ(countFactsFor(db_path_, user, ft, fk), 0);
}

TEST_F(UserFactsTest, RecordDecayedFirstCall)
{
    Database &db = open_db();
    const std::string user = "u2";
    const std::string ft   = "risk_preference"; // decayed = true
    const std::string fk   = "k2";

    Database::UserFactEvent ev;
    ev.user_id    = user;
    ev.fact_type  = ft;
    ev.fact_key   = fk;
    ev.payload    = "{}";
    ev.source     = "test";
    ev.created_at = nowUnix();

    const double expected_score = 0.42;
    auto fixed = [&](std::optional<double>, double) -> double {
        return expected_score;
    };

    double out_score = -1.0;
    ASSERT_TRUE(db.record_user_fact(ev, true, 3.14, fixed, "src", out_score));
    EXPECT_DOUBLE_EQ(out_score, expected_score);

    // Event row exists.
    EXPECT_EQ(countEventsFor(db_path_, user, ft, fk), 1);

    // Exactly one user_facts row.
    EXPECT_EQ(countFactsFor(db_path_, user, ft, fk), 1);

    // Load and verify fact_value.
    auto rows = db.load_user_facts_for_user(user, {ft});
    ASSERT_EQ(rows.size(), 1u);
    const auto &row = rows[0];
    EXPECT_EQ(row.fact_key, fk);
    rapidjson::Document val;
    val.Parse(row.fact_value.c_str());
    ASSERT_TRUE(val.HasMember("score"));
    EXPECT_NEAR(val["score"].GetDouble(), expected_score, 1e-9);
}

TEST_F(UserFactsTest, RecordDecayedSecondCallUpdate)
{
    Database &db = open_db();
    const std::string user = "u3";
    const std::string ft   = "market_region";
    const std::string fk   = "k3";

    Database::UserFactEvent ev;
    ev.user_id    = user;
    ev.fact_type  = ft;
    ev.fact_key   = fk;
    ev.payload    = "{}";
    ev.source     = "first";
    ev.created_at = nowUnix();

    const double first_score = 0.42;
    bool first_called = false;
    auto fn1 = [&](std::optional<double>, double) -> double {
        first_called = true;
        return first_score;
    };

    double out1 = -1.0;
    ASSERT_TRUE(db.record_user_fact(ev, true, 3.0, fn1, "first", out1));
    EXPECT_TRUE(first_called);
    EXPECT_DOUBLE_EQ(out1, first_score);

    // Second call: compute_fn should see old_score == first_score.
    bool second_called = false;
    double captured_old = -1.0;
    const double second_score = 0.77;
    auto fn2 = [&](std::optional<double> old, double) -> double {
        second_called = true;
        if (old.has_value()) captured_old = *old;
        return second_score;
    };

    double out2 = -1.0;
    ev.source = "second";
    ASSERT_TRUE(db.record_user_fact(ev, true, 5.0, fn2, "second", out2));
    EXPECT_TRUE(second_called);
    EXPECT_DOUBLE_EQ(out2, second_score);
    EXPECT_NEAR(captured_old, first_score, 1e-9);

    // Verify user_facts row: score is second_score, updated_at > created_at.
    auto rows = db.load_user_facts_for_user(user, {ft});
    ASSERT_EQ(rows.size(), 1u);
    const auto &row = rows[0];
    EXPECT_EQ(row.fact_key, fk);
    rapidjson::Document val;
    val.Parse(row.fact_value.c_str());
    ASSERT_TRUE(val.HasMember("score"));
    EXPECT_NEAR(val["score"].GetDouble(), second_score, 1e-9);
    // NOT EXPECT_GT: created_at/updated_at are int64_t Unix SECONDS with
    // no injected clock anywhere in this codebase, and these two calls
    // happen back-to-back with no delay — landing in the same
    // wall-clock second is legitimate, not a bug in record_user_fact.
    // The real invariant is "never goes backwards."
    EXPECT_GE(row.updated_at, row.created_at);
}

TEST_F(UserFactsTest, LoadUserFactsFiltersByTypeAndUser)
{
    Database &db = open_db();

    auto make_event = [&](const std::string &user,
                          const std::string &ftype,
                          const std::string &fkey) {
        Database::UserFactEvent ev;
        ev.user_id    = user;
        ev.fact_type  = ftype;
        ev.fact_key   = fkey;
        ev.payload    = "{}";
        ev.source     = "test";
        ev.created_at = nowUnix();
        return ev;
    };

    // Insert two facts for user A.
    double dummy = 0.0;
    auto returns1 = [](std::optional<double>, double) -> double { return 0.1; };

    ASSERT_TRUE(db.record_user_fact(make_event("A", "category_interest", "k1"),
                                    true, 0.5, returns1, "s", dummy));
    ASSERT_TRUE(db.record_user_fact(make_event("A", "risk_preference", "k2"),
                                    true, 0.5, returns1, "s", dummy));

    // Insert a fact for user B.
    ASSERT_TRUE(db.record_user_fact(make_event("B", "category_interest", "k3"),
                                    true, 0.5, returns1, "s", dummy));

    // Empty filter → all of user A, none of user B.
    auto allA = db.load_user_facts_for_user("A", {});
    ASSERT_EQ(allA.size(), 2u);
    // Filter for one type.
    auto oneType = db.load_user_facts_for_user("A", {"category_interest"});
    ASSERT_EQ(oneType.size(), 1u);
    EXPECT_EQ(oneType[0].fact_type, "category_interest");

    // User B should not see any of user A's facts.
    auto bFacts = db.load_user_facts_for_user("B", {});
    ASSERT_EQ(bFacts.size(), 1u);
    for (const auto &f : bFacts) {
        EXPECT_EQ(f.user_id, "B");
    }
}
