#include "database/database.hpp"
#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace agentos {

struct Database::Impl {
    sqlite3* db = nullptr;
    std::string db_path;
};

Database::Database(const std::string& db_path)
    : impl_(std::make_unique<Impl>())
{
    impl_->db_path = db_path;
}

Database::~Database() {
    close();
}

bool Database::open() {
    if (impl_->db) {
        spdlog::warn("[database] already open");
        return true;
    }
    int rc = sqlite3_open(impl_->db_path.c_str(), &impl_->db);
    if (rc != SQLITE_OK) {
        spdlog::error("[database] open failed: {}", sqlite3_errmsg(impl_->db));
        return false;
    }
    // Enable WAL mode for better concurrency
    const char* wal_sql = "PRAGMA journal_mode=WAL";
    sqlite3_exec(impl_->db, wal_sql, nullptr, nullptr, nullptr);
    // Create tables if not exist
    const char* create_sql = R"(
        CREATE TABLE IF NOT EXISTS jobs (
            id TEXT PRIMARY KEY,
            phase TEXT NOT NULL DEFAULT 'planning',
            payload TEXT,
            plan TEXT,
            updated_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS tasks (
            id TEXT PRIMARY KEY,
            job_id TEXT NOT NULL,
            agent_id TEXT,
            method TEXT,
            params TEXT,
            status TEXT DEFAULT 'pending',
            FOREIGN KEY (job_id) REFERENCES jobs(id)
        );
    )";
    char* errmsg = nullptr;
    rc = sqlite3_exec(impl_->db, create_sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        spdlog::error("[database] create tables failed: {}", errmsg);
        sqlite3_free(errmsg);
        return false;
    }
    spdlog::info("[database] opened {}", impl_->db_path);
    return true;
}

void Database::close() {
    if (impl_->db) {
        sqlite3_close(impl_->db);
        impl_->db = nullptr;
        spdlog::info("[database] closed");
    }
}

bool Database::is_open() const {
    return impl_->db != nullptr;
}

sqlite3* Database::db_handle() const {
    return impl_->db;
}

void Database::store_job(const Task& task) {
    if (!impl_->db) return;
    const char* sql = R"(
        INSERT OR REPLACE INTO jobs (id, phase, payload, updated_at)
        VALUES (?, ?, ?, ?)
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[database] store_job prepare: {}", sqlite3_errmsg(impl_->db));
        return;
    }
    sqlite3_bind_text(stmt, 1, task.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, "planning", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, task.input_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, std::chrono::system_clock::now().time_since_epoch().count());
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("[database] store_job step: {}", sqlite3_errmsg(impl_->db));
    }
    sqlite3_finalize(stmt);
}

void Database::update_job_phase(const TaskId& id, const std::string& phase) {
    if (!impl_->db) return;
    const char* sql = "UPDATE jobs SET phase = ?, updated_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[database] update_job_phase prepare: {}", sqlite3_errmsg(impl_->db));
        return;
    }
    sqlite3_bind_text(stmt, 1, phase.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, std::chrono::system_clock::now().time_since_epoch().count());
    sqlite3_bind_text(stmt, 3, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("[database] update_job_phase step: {}", sqlite3_errmsg(impl_->db));
    }
    sqlite3_finalize(stmt);
}

void Database::update_job_plan(const TaskId& id, const std::string& plan_json) {
    if (!impl_->db) return;
    const char* sql = "UPDATE jobs SET plan = ?, updated_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[database] update_job_plan prepare: {}", sqlite3_errmsg(impl_->db));
        return;
    }
    sqlite3_bind_text(stmt, 1, plan_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, std::chrono::system_clock::now().time_since_epoch().count());
    sqlite3_bind_text(stmt, 3, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("[database] update_job_plan step: {}", sqlite3_errmsg(impl_->db));
    }
    sqlite3_finalize(stmt);
}

void Database::store_task(const TaskId& job_id, const PlanStep& step) {
    if (!impl_->db) return;
    const char* sql = R"(
        INSERT OR REPLACE INTO tasks (id, job_id, agent_id, method, params, status)
        VALUES (?, ?, ?, ?, ?, 'pending')
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[database] store_task prepare: {}", sqlite3_errmsg(impl_->db));
        return;
    }
    sqlite3_bind_text(stmt, 1, step.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, job_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, "", -1, SQLITE_STATIC); // agent_id unknown
    sqlite3_bind_text(stmt, 4, step.command.c_str(), -1, SQLITE_TRANSIENT);
    // Serialize args as JSON
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    for (const auto& [key, val] : step.args) {
        w.Key(key.c_str());
        w.String(val.c_str());
    }
    w.EndObject();
    std::string args_json = buf.GetString();
    sqlite3_bind_text(stmt, 5, args_json.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("[database] store_task step: {}", sqlite3_errmsg(impl_->db));
    }
    sqlite3_finalize(stmt);
}

std::string Database::load_plan_json(const TaskId& job_id) {
    if (!impl_->db) return "";
    const char* sql = "SELECT plan FROM jobs WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[database] load_plan_json prepare: {}", sqlite3_errmsg(impl_->db));
        return "";
    }
    sqlite3_bind_text(stmt, 1, job_id.c_str(), -1, SQLITE_TRANSIENT);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) result = text;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<Database::InFlightJob> Database::resume_in_flight() {
    std::vector<InFlightJob> jobs;
    if (!impl_->db) return jobs;
    const char* sql = "SELECT id, plan FROM jobs WHERE phase NOT IN ('done','failed')";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[database] resume_in_flight prepare: {}", sqlite3_errmsg(impl_->db));
        return jobs;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        InFlightJob j;
        const char* id_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (id_text) j.job_id = id_text;
        const char* plan_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (plan_text) j.plan_json = plan_text;
        jobs.push_back(std::move(j));
    }
    sqlite3_finalize(stmt);
    return jobs;
}

} // namespace agentos
