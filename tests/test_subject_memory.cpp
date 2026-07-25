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

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "agentos/database.h"

using namespace agentos;

namespace {

// ---------- raw-count helpers (separate connection) ----------

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
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}

static int countSubjectUnits(const std::string &dbPath,
                             const std::string &subjectId)
{
    const std::string sql = "SELECT COUNT(*) FROM subject_units WHERE subject_id=?";
    return rawCountRows(dbPath, sql, {subjectId});
}

static int countSubjectMemory(const std::string &dbPath,
                              const std::string &subjectId)
{
    const std::string sql = "SELECT COUNT(*) FROM subject_memory WHERE subject_id=?";
    return rawCountRows(dbPath, sql, {subjectId});
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class SubjectMemoryTest : public ::testing::Test
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
        char tmp[] = "/tmp/agentos_subject_mem_test_XXXXXX";
        int fd = mkstemp(tmp);
        ASSERT_NE(fd, -1) << "mkstemp failed";
        close(fd);
        db_path_ = tmp;

        char home_tmp[] = "/tmp/agentos_subject_mem_home_XXXXXX";
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

    // Helper to read a single unit's status via raw SQL (not via Database's own methods).
    int getUnitStatus(const std::string &subjectId,
                      const std::string &unitRef) const
    {
        sqlite3 *raw = nullptr;
        EXPECT_EQ(sqlite3_open(db_path_.c_str(), &raw), SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        const std::string sql =
            "SELECT status FROM subject_units WHERE subject_id=? AND unit_ref=?";
        EXPECT_EQ(sqlite3_prepare_v2(raw, sql.c_str(), -1, &stmt, nullptr),
                  SQLITE_OK);
        sqlite3_bind_text(stmt, 1, subjectId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, unitRef.c_str(), -1, SQLITE_TRANSIENT);
        int st = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            st = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        sqlite3_close(raw);
        return st;
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(SubjectMemoryTest, InsertAndLoadSubject)
{
    Database &db = open_db();

    Database::SubjectRow row;
    row.subject_id   = "s1";
    row.user_id      = "userX";
    row.subject_type = "topic";
    row.unit_type    = "file";
    row.title        = "Hello";
    row.created_at   = nowUnix();
    row.updated_at   = row.created_at;

    db.insert_subject(row);

    auto loaded = db.load_subject("s1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->subject_id, "s1");
    EXPECT_EQ(loaded->user_id, "userX");
    EXPECT_EQ(loaded->subject_type, "topic");
    EXPECT_EQ(loaded->unit_type, "file");
    EXPECT_EQ(loaded->title, "Hello");
}

TEST_F(SubjectMemoryTest, LoadSubjectNonexistentReturnsNullopt)
{
    Database &db = open_db();
    EXPECT_FALSE(db.load_subject("no_such_id").has_value());
}

TEST_F(SubjectMemoryTest, PopulateSubjectUnitsInsertsAll)
{
    Database &db = open_db();
    Database::SubjectRow row{"s2", "u1", "type", "file", "Test", nowUnix(), nowUnix()};
    db.insert_subject(row);

    std::vector<std::string> refs{"a.txt", "b.txt", "c.txt"};
    int inserted = 0, already = 0;
    db.populate_subject_units("s2", refs, inserted, already);
    EXPECT_EQ(inserted, 3);
    EXPECT_EQ(already, 0);

    auto units = db.next_pending_subject_units("s2", 100);
    ASSERT_EQ(units.size(), 3u);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(units[i].unit_index, i);
        EXPECT_EQ(units[i].status, 0); // pending
    }
}

// ADR-035 idempotency test: re-populating with overlapping refs does not reset
// a completed unit back to pending, and inserted/known counts reflect the true delta.
TEST_F(SubjectMemoryTest, PopulateIdempotentDoesNotResetCompletedUnit)
{
    Database &db = open_db();
    Database::SubjectRow row{"s3", "u2", "type", "file", "Idempotent", nowUnix(), nowUnix()};
    db.insert_subject(row);

    // First populate: a, b, c
    std::vector<std::string> first{"a", "b", "c"};
    int ins1 = 0, known1 = 0;
    db.populate_subject_units("s3", first, ins1, known1);
    ASSERT_EQ(ins1, 3);
    ASSERT_EQ(known1, 0);

    // Complete unit "b" (unit_index == 1)
    ASSERT_TRUE(db.complete_subject_units("s3", {1}));
    EXPECT_EQ(getUnitStatus("s3", "b"), 1); // completed

    // Second populate with overlapping ref "b" + new "d"
    std::vector<std::string> second{"b", "d"};
    int ins2 = 0, known2 = 0;
    db.populate_subject_units("s3", second, ins2, known2);
    EXPECT_EQ(ins2, 1);   // only "d" is truly new
    EXPECT_EQ(known2, 1); // "b" is already known (and completed)

    // The already-completed unit "b" must still have status=1.
    EXPECT_EQ(getUnitStatus("s3", "b"), 1)
        << "Re-populating with a completed unit_ref must not reset its status to pending";
}

TEST_F(SubjectMemoryTest, NextPendingRespectsLimitAndOrder)
{
    Database &db = open_db();
    Database::SubjectRow row{"s4", "u3", "type", "file", "Limit", nowUnix(), nowUnix()};
    db.insert_subject(row);

    std::vector<std::string> refs{"x", "y", "z"};
    int ins = 0, kn = 0;
    db.populate_subject_units("s4", refs, ins, kn);
    ASSERT_EQ(ins, 3);

    auto first = db.next_pending_subject_units("s4", 2);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0].unit_ref, "x");
    EXPECT_EQ(first[1].unit_ref, "y");

    // Complete unit index 0 (x)
    ASSERT_TRUE(db.complete_subject_units("s4", {0}));

    auto second = db.next_pending_subject_units("s4", 100);
    ASSERT_EQ(second.size(), 2u);
    EXPECT_EQ(second[0].unit_ref, "y");
    EXPECT_EQ(second[1].unit_ref, "z");
}

TEST_F(SubjectMemoryTest, GetSubjectProgress)
{
    Database &db = open_db();
    Database::SubjectRow row{"s5", "u4", "type", "file", "Progress", nowUnix(), nowUnix()};
    db.insert_subject(row);

    std::vector<std::string> refs{"p", "q", "r", "s"};
    int ins = 0, kn = 0;
    db.populate_subject_units("s5", refs, ins, kn);
    ASSERT_EQ(ins, 4);

    auto prog0 = db.get_subject_progress("s5");
    EXPECT_EQ(prog0.total, 4);
    EXPECT_EQ(prog0.completed, 0);

    ASSERT_TRUE(db.complete_subject_units("s5", {1, 3})); // q and s

    auto prog1 = db.get_subject_progress("s5");
    EXPECT_EQ(prog1.total, 4);
    EXPECT_EQ(prog1.completed, 2);
}

TEST_F(SubjectMemoryTest, UpsertSubjectMemoryInitial)
{
    Database &db = open_db();
    Database::SubjectRow row{"s6", "u5", "type", "file", "Memory", nowUnix(), nowUnix()};
    db.insert_subject(row);

    Database::SubjectMemoryRow mem;
    mem.subject_id   = "s6";
    mem.entry_key    = "key1";
    mem.entry_value  = R"({"data":"hello"})";
    mem.related_asset_ids = "[]";
    mem.source_job_id = "job1";
    mem.created_at   = nowUnix();
    mem.updated_at   = mem.created_at;

    db.upsert_subject_memory(mem);
    EXPECT_EQ(mem.revision, 1);
    EXPECT_EQ(countSubjectMemory(db_path_, "s6"), 1);
}

TEST_F(SubjectMemoryTest, UpsertSubjectMemorySecondCallIncrementsRevision)
{
    Database &db = open_db();
    Database::SubjectRow row{"s7", "u6", "type", "file", "Memory2", nowUnix(), nowUnix()};
    db.insert_subject(row);

    Database::SubjectMemoryRow mem;
    mem.subject_id   = "s7";
    mem.entry_key    = "keyA";
    mem.entry_value  = R"({"v":"first"})";
    mem.related_asset_ids = "[]";
    mem.source_job_id = "jobA";
    mem.created_at   = nowUnix();
    mem.updated_at   = mem.created_at;

    db.upsert_subject_memory(mem);
    ASSERT_EQ(mem.revision, 1);

    // Store created_at from the persisted row (raw query) so we can assert it
    // doesn't change.
    sqlite3 *raw = nullptr;
    ASSERT_EQ(sqlite3_open(db_path_.c_str(), &raw), SQLITE_OK);
    sqlite3_stmt *st = nullptr;
    const std::string selSql =
        "SELECT created_at, entry_value FROM subject_memory "
        "WHERE subject_id=? AND entry_key=?";
    ASSERT_EQ(sqlite3_prepare_v2(raw, selSql.c_str(), -1, &st, nullptr),
              SQLITE_OK);
    sqlite3_bind_text(st, 1, "s7", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, "keyA", -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t originalCreated = sqlite3_column_int64(st, 0);
    std::string originalValue = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    sqlite3_finalize(st);
    sqlite3_close(raw);

    // Second upsert with different value.
    mem.entry_value  = R"({"v":"second"})";
    mem.updated_at   = nowUnix();
    db.upsert_subject_memory(mem);
    EXPECT_EQ(mem.revision, 2);

    // Verify that created_at stayed unchanged and updated_at moved forward.
    {
        sqlite3 *raw2 = nullptr;
        ASSERT_EQ(sqlite3_open(db_path_.c_str(), &raw2), SQLITE_OK);
        sqlite3_stmt *st2 = nullptr;
        const std::string selSql2 =
            "SELECT created_at, updated_at FROM subject_memory "
            "WHERE subject_id=? AND entry_key=?";
        ASSERT_EQ(sqlite3_prepare_v2(raw2, selSql2.c_str(), -1, &st2, nullptr),
                  SQLITE_OK);
        sqlite3_bind_text(st2, 1, "s7", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st2, 2, "keyA", -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(st2), SQLITE_ROW);
        int64_t persistedCreated = sqlite3_column_int64(st2, 0);
        int64_t persistedUpdated = sqlite3_column_int64(st2, 1);
        EXPECT_EQ(persistedCreated, originalCreated);   // created_at unchanged
        // NOT EXPECT_GT: created_at/updated_at are int64_t Unix SECONDS
        // (project convention, no sub-second or injected-clock support
        // anywhere in this codebase), and these two calls happen
        // back-to-back with no delay, so they can legitimately land in
        // the same wall-clock second — that is correct behavior, not a
        // bug in upsert_subject_memory. The invariant this test actually
        // needs to guard is "updated_at never goes backwards relative to
        // created_at," which >= verifies without being flaky.
        EXPECT_GE(persistedUpdated, originalCreated);
        sqlite3_finalize(st2);
        sqlite3_close(raw2);
    }

    // Instead rely on simple check:
    // We'll load via query_subject_memory and check revision & values.
    auto rows = db.query_subject_memory("s7", /*key_prefix=*/std::nullopt, 10, std::nullopt);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].revision, 2);
    EXPECT_NE(rows[0].entry_value, originalValue)
        << "Second upsert must update entry_value";
    // Created_at should not have changed.
    // (Cannot check directly but test does enough.)
}

TEST_F(SubjectMemoryTest, QuerySubjectMemoryPagination)
{
    Database &db = open_db();
    Database::SubjectRow row{"s8", "u7", "type", "file", "Paginate", nowUnix(), nowUnix()};
    db.insert_subject(row);

    // Insert 5 entries.
    for (int i = 1; i <= 5; ++i) {
        Database::SubjectMemoryRow mem;
        mem.subject_id   = "s8";
        mem.entry_key    = "k" + std::to_string(i);
        mem.entry_value  = "{}";
        mem.related_asset_ids = "[]";
        mem.source_job_id = "job";
        mem.created_at   = nowUnix();
        mem.updated_at   = mem.created_at;
        db.upsert_subject_memory(mem);
    }

    const int limit = 2;
    std::optional<std::string> cursor;
    std::vector<std::string> seen;

    while (true) {
        auto rows = db.query_subject_memory("s8", std::nullopt, limit, cursor);
        if (rows.empty()) {
            // Note: no assertion on `cursor` itself here — it's just this
            // test's own loop-local bookmark (the last key seen), and it
            // legitimately still holds a real value at this point (e.g.
            // "k5"). Database::query_subject_memory has no separate
            // next_cursor output of its own to check; that field only
            // exists at the RPC layer (cmd_subject_memory_query), which
            // isn't under test here. What actually matters — that all 5
            // entries were seen exactly once, in order — is asserted below.
            break;
        }
        for (const auto &r : rows) {
            seen.push_back(r.entry_key);
        }
        EXPECT_LE(rows.size(), static_cast<size_t>(limit));
        // Always carry the cursor forward to the last key seen — do NOT
        // reset it to std::nullopt here. query_subject_memory treats an
        // absent cursor as "start from the beginning," so resetting it on
        // a short (last) page makes the next call re-fetch page 1 instead
        // of correctly returning empty — that reset is exactly what
        // turned this into an infinite loop (seen kept growing forever,
        // the while(true) never reached the rows.empty() break). The loop
        // terminates correctly on its own once a call with a real cursor
        // returns zero rows; no "is this the last page" guess is needed.
        cursor = rows.back().entry_key;
    }

    ASSERT_EQ(seen.size(), 5u);
    // Should be sorted naturally as "k1","k2","k3","k4","k5".
    EXPECT_EQ(seen[0], "k1");
    EXPECT_EQ(seen[1], "k2");
    EXPECT_EQ(seen[2], "k3");
    EXPECT_EQ(seen[3], "k4");
    EXPECT_EQ(seen[4], "k5");
}

TEST_F(SubjectMemoryTest, QuerySubjectMemoryKeyPrefix)
{
    Database &db = open_db();
    Database::SubjectRow row{"s9", "u8", "type", "file", "Prefix", nowUnix(), nowUnix()};
    db.insert_subject(row);

    auto ins = [&](const std::string &key) {
        Database::SubjectMemoryRow mem;
        mem.subject_id   = "s9";
        mem.entry_key    = key;
        mem.entry_value  = "{}";
        mem.related_asset_ids = "[]";
        mem.source_job_id = "job";
        mem.created_at   = nowUnix();
        mem.updated_at   = mem.created_at;
        db.upsert_subject_memory(mem);
    };

    ins("alpha");
    ins("apple");
    ins("beta");

    auto all = db.query_subject_memory("s9", std::nullopt, 100, std::nullopt);
    ASSERT_EQ(all.size(), 3u);

    // NOT "ap": "alpha" is a-l-p-h-a — it does not actually share an
    // "ap" prefix with "apple" ("al" vs "ap"), only "apple" does. Query
    // for "a" instead, which genuinely matches both "alpha" and "apple"
    // while still excluding "beta" — the ordering assertions below
    // (alpha < apple alphabetically) hold either way.
    auto ap = db.query_subject_memory("s9", std::string("a"), 100, std::nullopt);
    ASSERT_EQ(ap.size(), 2u);
    EXPECT_EQ(ap[0].entry_key, "alpha"); // "alpha" < "apple" alphabetically
    EXPECT_EQ(ap[1].entry_key, "apple");
}

TEST_F(SubjectMemoryTest, QueryEmptySubjectReturnsEmpty)
{
    Database &db = open_db();
    auto rows = db.query_subject_memory("no_such_subject", std::nullopt, 10, std::nullopt);
    EXPECT_TRUE(rows.empty());
}

TEST_F(SubjectMemoryTest, DifferentUsersIsolation)
{
    Database &db = open_db();

    // User Alice subject + units + memory.
    Database::SubjectRow alice{"suba", "alice", "type", "file", "Alice", nowUnix(), nowUnix()};
    db.insert_subject(alice);
    std::vector<std::string> refsA{"a.txt"};
    int insA = 0, knA = 0;
    db.populate_subject_units("suba", refsA, insA, knA);
    Database::SubjectMemoryRow memA;
    memA.subject_id   = "suba";
    memA.entry_key    = "memA";
    memA.entry_value  = "{}";
    memA.related_asset_ids = "[]";
    memA.source_job_id = "jA";
    memA.created_at   = nowUnix();
    memA.updated_at   = memA.created_at;
    db.upsert_subject_memory(memA);

    // User Bob subject + units + memory.
    Database::SubjectRow bob{"subb", "bob", "type", "file", "Bob", nowUnix(), nowUnix()};
    db.insert_subject(bob);
    std::vector<std::string> refsB{"b.txt"};
    int insB = 0, knB = 0;
    db.populate_subject_units("subb", refsB, insB, knB);
    Database::SubjectMemoryRow memB;
    memB.subject_id   = "subb";
    memB.entry_key    = "memB";
    memB.entry_value  = "{}";
    memB.related_asset_ids = "[]";
    memB.source_job_id = "jB";
    memB.created_at   = nowUnix();
    memB.updated_at   = memB.created_at;
    db.upsert_subject_memory(memB);

    // Alice's domain should NOT see Bob's subject or units.
    EXPECT_FALSE(db.load_subject("subb")->user_id == "alice"); // Bob subject has user bob.
    auto bobSubject = db.load_subject("subb");
    ASSERT_TRUE(bobSubject.has_value());
    EXPECT_EQ(bobSubject->user_id, "bob");   // not alice

    // Units for Alice's subject only.
    auto unitsA = db.next_pending_subject_units("suba", 100);
    ASSERT_EQ(unitsA.size(), 1u);
    EXPECT_EQ(unitsA[0].unit_ref, "a.txt");

    // Memory for Alice only.
    auto memAlice = db.query_subject_memory("suba", std::nullopt, 10, std::nullopt);
    ASSERT_EQ(memAlice.size(), 1u);
    EXPECT_EQ(memAlice[0].entry_key, "memA");
}
