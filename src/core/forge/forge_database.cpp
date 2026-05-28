#include "agentos/forge/forge_database.h"
#include "agentos/database/database.h"
#include <sqlite3.h>
#include <spdlog/spdlog.h>

namespace agentos {

ForgeDatabase::ForgeDatabase(Database& db) : db_(db) {}

void ForgeDatabase::create_tables() {
    sqlite3* sqlite = db_.db_handle();
    if (!sqlite) {
        spdlog::error("[forge_db] database not open");
        return;
    }
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS forge_jobs (
            id            TEXT PRIMARY KEY,
            method        TEXT NOT NULL,
            requirement   TEXT NOT NULL,
            attempt       INTEGER DEFAULT 0,
            max_attempts  INTEGER DEFAULT 3,
            phase         TEXT NOT NULL,
            last_code     TEXT,
            last_feedback TEXT,
            last_code_path TEXT,
            created_at    INTEGER NOT NULL,
            updated_at    INTEGER NOT NULL
        );
    )";
    char* errmsg = nullptr;
    if (sqlite3_exec(sqlite, sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        spdlog::error("[forge_db] create_tables: {}", errmsg);
        sqlite3_free(errmsg);
    }
}

void ForgeDatabase::insert_job(const ForgeJob& job) {
    sqlite3* sqlite = db_.db_handle();
    if (!sqlite) return;
    const char* sql = R"(
        INSERT INTO forge_jobs (id, method, requirement, attempt, max_attempts, phase, last_code, last_feedback, last_code_path, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(sqlite, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[forge_db] insert_job prepare: {}", sqlite3_errmsg(sqlite));
        return;
    }
    sqlite3_bind_text(stmt, 1, job.id.value().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, job.method.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, job.requirement.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, job.attempt);
    sqlite3_bind_int(stmt, 5, job.max_attempts);
    sqlite3_bind_text(stmt, 6, job.phase.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, job.last_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, job.last_feedback.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, job.last_code_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 10, job.created_at);
    sqlite3_bind_int64(stmt, 11, job.updated_at);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("[forge_db] insert_job step: {}", sqlite3_errmsg(sqlite));
    }
    sqlite3_finalize(stmt);
}

void ForgeDatabase::update_job(const ForgeJob& job) {
    sqlite3* sqlite = db_.db_handle();
    if (!sqlite) return;
    const char* sql = R"(
        UPDATE forge_jobs SET method=?, requirement=?, attempt=?, max_attempts=?, phase=?, last_code=?, last_feedback=?, last_code_path=?, updated_at=?
        WHERE id=?
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(sqlite, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[forge_db] update_job prepare: {}", sqlite3_errmsg(sqlite));
        return;
    }
    sqlite3_bind_text(stmt, 1, job.method.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, job.requirement.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, job.attempt);
    sqlite3_bind_int(stmt, 4, job.max_attempts);
    sqlite3_bind_text(stmt, 5, job.phase.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, job.last_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, job.last_feedback.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, job.last_code_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 9, job.updated_at);
    sqlite3_bind_text(stmt, 10, job.id.value().c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("[forge_db] update_job step: {}", sqlite3_errmsg(sqlite));
    }
    sqlite3_finalize(stmt);
}

std::optional<ForgeJob> ForgeDatabase::get_job(const std::string& id) {
    sqlite3* sqlite = db_.db_handle();
    if (!sqlite) return std::nullopt;
    const char* sql = "SELECT id, method, requirement, attempt, max_attempts, phase, last_code, last_feedback, last_code_path, created_at, updated_at FROM forge_jobs WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(sqlite, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[forge_db] get_job prepare: {}", sqlite3_errmsg(sqlite));
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ForgeJob job;
        job.id = ForgeJobId(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        job.method = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        job.requirement = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        job.attempt = sqlite3_column_int(stmt, 3);
        job.max_attempts = sqlite3_column_int(stmt, 4);
        job.phase = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        job.last_code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        job.last_feedback = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        job.last_code_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        job.created_at = sqlite3_column_int64(stmt, 9);
        job.updated_at = sqlite3_column_int64(stmt, 10);
        sqlite3_finalize(stmt);
        return job;
    }
    sqlite3_finalize(stmt);
    return std::nullopt;
}

std::vector<ForgeJob> ForgeDatabase::get_jobs_by_phase(const std::string& phase) {
    std::vector<ForgeJob> jobs;
    sqlite3* sqlite = db_.db_handle();
    if (!sqlite) return jobs;
    const char* sql = "SELECT id, method, requirement, attempt, max_attempts, phase, last_code, last_feedback, created_at, updated_at FROM forge_jobs WHERE phase=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(sqlite, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[forge_db] get_jobs_by_phase prepare: {}", sqlite3_errmsg(sqlite));
        return jobs;
    }
    sqlite3_bind_text(stmt, 1, phase.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ForgeJob job;
        job.id = ForgeJobId(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        job.method = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        job.requirement = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        job.attempt = sqlite3_column_int(stmt, 3);
        job.max_attempts = sqlite3_column_int(stmt, 4);
        job.phase = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        job.last_code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        job.last_feedback = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        job.created_at = sqlite3_column_int64(stmt, 8);
        job.updated_at = sqlite3_column_int64(stmt, 9);
        jobs.push_back(std::move(job));
    }
    sqlite3_finalize(stmt);
    return jobs;
}

std::vector<ForgeJob> ForgeDatabase::get_all_jobs() {
    std::vector<ForgeJob> jobs;
    sqlite3* sqlite = db_.db_handle();
    if (!sqlite) return jobs;
    const char* sql = "SELECT id, method, requirement, attempt, max_attempts, phase, last_code, last_feedback, last_code_path, created_at, updated_at FROM forge_jobs";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(sqlite, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[forge_db] get_all_jobs prepare: {}", sqlite3_errmsg(sqlite));
        return jobs;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ForgeJob job;
        job.id = ForgeJobId(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        job.method = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        job.requirement = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        job.attempt = sqlite3_column_int(stmt, 3);
        job.max_attempts = sqlite3_column_int(stmt, 4);
        job.phase = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        job.last_code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        job.last_feedback = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        job.last_code_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        job.created_at = sqlite3_column_int64(stmt, 9);
        job.updated_at = sqlite3_column_int64(stmt, 10);
        jobs.push_back(std::move(job));
    }
    sqlite3_finalize(stmt);
    return jobs;
}

} // namespace agentos
