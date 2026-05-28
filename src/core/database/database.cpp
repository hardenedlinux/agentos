#include "agentos/database/database.h"
#include "agentos/home_init.h"
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
    if (!db_path.empty()) {
        impl_->db_path = db_path;
    } else {
        impl_->db_path = (agentos_home() / "agentos.db").string();
    }
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
        CREATE TABLE IF NOT EXISTS worker_runs (
            run_id     TEXT PRIMARY KEY,
            worker_id  TEXT NOT NULL,
            pid        INTEGER NOT NULL,
            started_at INTEGER NOT NULL,
            ended_at   INTEGER,
            exit_code  INTEGER,
            status     TEXT NOT NULL DEFAULT 'running',
            layer_path TEXT NOT NULL,
            log_path   TEXT NOT NULL
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
    sqlite3_bind_text(stmt, 1, task.id.value().c_str(), -1, SQLITE_TRANSIENT);
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
    sqlite3_bind_text(stmt, 3, id.value().c_str(), -1, SQLITE_TRANSIENT);
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
    sqlite3_bind_text(stmt, 3, id.value().c_str(), -1, SQLITE_TRANSIENT);
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
    sqlite3_bind_text(stmt, 2, job_id.value().c_str(), -1, SQLITE_TRANSIENT);
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
    sqlite3_bind_text(stmt, 1, job_id.value().c_str(), -1, SQLITE_TRANSIENT);
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
        if (id_text) j.job_id = TaskId(id_text);
        const char* plan_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (plan_text) j.plan_json = plan_text;
        jobs.push_back(std::move(j));
    }
    sqlite3_finalize(stmt);
    return jobs;
}

void Database::insert_worker_run(const WorkerRun& run) {
    if (!impl_->db) return;
    const char* sql = R"(
        INSERT INTO worker_runs (run_id, worker_id, pid, started_at, ended_at, exit_code, status, layer_path, log_path)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[database] insert_worker_run prepare: {}", sqlite3_errmsg(impl_->db));
        return;
    }
    sqlite3_bind_text(stmt, 1, run.run_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, run.worker_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, run.pid);
    sqlite3_bind_int64(stmt, 4, run.started_at);
    if (run.ended_at == 0)
        sqlite3_bind_null(stmt, 5);
    else
        sqlite3_bind_int64(stmt, 5, run.ended_at);
    if (run.exit_code == -1)
        sqlite3_bind_null(stmt, 6);
    else
        sqlite3_bind_int(stmt, 6, run.exit_code);
    sqlite3_bind_text(stmt, 7, run.status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, run.layer_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, run.log_path.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("[database] insert_worker_run step: {}", sqlite3_errmsg(impl_->db));
    }
    sqlite3_finalize(stmt);
}

void Database::update_worker_run(const WorkerRun& run) {
    if (!impl_->db) return;
    const char* sql = "UPDATE worker_runs SET ended_at=?, exit_code=?, status=? WHERE run_id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[database] update_worker_run prepare: {}", sqlite3_errmsg(impl_->db));
        return;
    }
    if (run.ended_at == 0)
        sqlite3_bind_null(stmt, 1);
    else
        sqlite3_bind_int64(stmt, 1, run.ended_at);
    if (run.exit_code == -1)
        sqlite3_bind_null(stmt, 2);
    else
        sqlite3_bind_int(stmt, 2, run.exit_code);
    sqlite3_bind_text(stmt, 3, run.status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, run.run_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("[database] update_worker_run step: {}", sqlite3_errmsg(impl_->db));
    }
    sqlite3_finalize(stmt);
}

std::vector<WorkerRun> Database::get_active_worker_runs() {
    std::vector<WorkerRun> runs;
    if (!impl_->db) return runs;
    const char* sql = "SELECT run_id, worker_id, pid, started_at, ended_at, exit_code, status, layer_path, log_path FROM worker_runs WHERE status='running'";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[database] get_active_worker_runs prepare: {}", sqlite3_errmsg(impl_->db));
        return runs;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WorkerRun run;
        run.run_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        run.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        run.pid = sqlite3_column_int(stmt, 2);
        run.started_at = sqlite3_column_int64(stmt, 3);
        run.ended_at = sqlite3_column_int64(stmt, 4);
        run.exit_code = sqlite3_column_int(stmt, 5);
        run.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        run.layer_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        run.log_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        runs.push_back(std::move(run));
    }
    sqlite3_finalize(stmt);
    return runs;
}

std::vector<WorkerRun> Database::get_all_worker_runs() {
    std::vector<WorkerRun> runs;
    if (!impl_->db) return runs;
    const char* sql = "SELECT run_id, worker_id, pid, started_at, ended_at, exit_code, status, layer_path, log_path FROM worker_runs";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[database] get_all_worker_runs prepare: {}", sqlite3_errmsg(impl_->db));
        return runs;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WorkerRun run;
        run.run_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        run.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        run.pid = sqlite3_column_int(stmt, 2);
        run.started_at = sqlite3_column_int64(stmt, 3);
        run.ended_at = sqlite3_column_int64(stmt, 4);
        run.exit_code = sqlite3_column_int(stmt, 5);
        run.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        run.layer_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        run.log_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        runs.push_back(std::move(run));
    }
    sqlite3_finalize(stmt);
    return runs;
}

void Database::mark_all_running_as_crashed() {
    if (!impl_->db) return;
    const char* sql = "UPDATE worker_runs SET status='crashed', ended_at=? WHERE status='running'";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[database] mark_all_running_as_crashed prepare: {}", sqlite3_errmsg(impl_->db));
        return;
    }
    sqlite3_bind_int64(stmt, 1, std::chrono::system_clock::now().time_since_epoch().count());
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("[database] mark_all_running_as_crashed step: {}", sqlite3_errmsg(impl_->db));
    }
    sqlite3_finalize(stmt);
}

} // namespace agentos
