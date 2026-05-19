#include "agentos/orchestrator.h"
#include <spdlog/spdlog.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <sqlite3.h>
#include <filesystem>
#include <chrono>

namespace agentos {

Orchestrator::Orchestrator(Registry&    registry,
                           Verifier&    verifier,
                           Scheduler&   scheduler,
                           Dispatcher&  dispatcher,
                           const std::string& db_path)
    : registry_(registry)
    , verifier_(verifier)
    , scheduler_(scheduler)
    , dispatcher_(dispatcher)
    , db_(nullptr)
    , db_path_(db_path)
{
    if (init_db()) {
        resume_in_flight();
    }
}

Orchestrator::~Orchestrator() {
    close_db();
}

bool Orchestrator::init_db() {
    if (db_path_.empty()) {
        spdlog::warn("[orchestrator] no db_path, persistence disabled");
        return false;
    }
    // Ensure directory exists
    std::filesystem::path p(db_path_);
    std::filesystem::create_directories(p.parent_path());

    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        spdlog::error("[orchestrator] failed to open database: {}", sqlite3_errmsg(db_));
        return false;
    }

    // Enable WAL mode
    const char* pragmas[] = {
        "PRAGMA journal_mode=WAL;",
        "PRAGMA synchronous=NORMAL;",
        "PRAGMA foreign_keys=ON;",
        nullptr
    };
    for (int i=0; pragmas[i]; ++i) {
        char* err = nullptr;
        rc = sqlite3_exec(db_, pragmas[i], nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            spdlog::error("[orchestrator] pragma error: {}", err);
            sqlite3_free(err);
            sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }
    }

    // Create tables
    const char* create_sql = R"(
        CREATE TABLE IF NOT EXISTS jobs (
            id         TEXT PRIMARY KEY,
            phase      TEXT NOT NULL,
            payload    TEXT NOT NULL,
            plan       TEXT,
            updated_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS tasks (
            id       TEXT PRIMARY KEY,
            job_id   TEXT NOT NULL REFERENCES jobs(id),
            agent_id TEXT NOT NULL,
            method   TEXT NOT NULL,
            params   TEXT NOT NULL,
            result   TEXT,
            status   TEXT NOT NULL DEFAULT 'pending'
        );
    )";
    char* err = nullptr;
    rc = sqlite3_exec(db_, create_sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        spdlog::error("[orchestrator] create tables error: {}", err);
        sqlite3_free(err);
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    spdlog::info("[orchestrator] database initialized at {}", db_path_);
    return true;
}

void Orchestrator::close_db() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Orchestrator::store_job(const Task& task) {
    if (!db_) return;
    const char* sql = "INSERT OR REPLACE INTO jobs (id, phase, payload, updated_at) VALUES (?, ?, ?, ?)";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, task.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, "planning", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, task.input_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, std::chrono::system_clock::now().time_since_epoch().count());
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Orchestrator::update_job_phase(const TaskId& id, const std::string& phase) {
    if (!db_) return;
    const char* sql = "UPDATE jobs SET phase = ?, updated_at = ? WHERE id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, phase.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, std::chrono::system_clock::now().time_since_epoch().count());
    sqlite3_bind_text(stmt, 3, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Orchestrator::update_job_plan(const TaskId& id, const std::string& plan_json) {
    if (!db_) return;
    const char* sql = "UPDATE jobs SET plan = ?, updated_at = ? WHERE id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, plan_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, std::chrono::system_clock::now().time_since_epoch().count());
    sqlite3_bind_text(stmt, 3, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Orchestrator::store_task(const TaskId& job_id, const PlanStep& step) {
    if (!db_) return;
    const char* sql = "INSERT OR REPLACE INTO tasks (id, job_id, agent_id, method, params, status) VALUES (?, ?, ?, ?, ?, 'pending')";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, step.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, job_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, "", -1, SQLITE_STATIC); // agent_id unknown at this point
    sqlite3_bind_text(stmt, 4, step.command.c_str(), -1, SQLITE_TRANSIENT);
    // Serialise args as JSON
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    for (const auto& [k,v] : step.args) {
        w.Key(k.c_str());
        w.String(v.c_str());
    }
    w.EndObject();
    std::string args_json = buf.GetString();
    sqlite3_bind_text(stmt, 5, args_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Orchestrator::update_task_result(const TaskId& task_id, const std::string& result, const std::string& status) {
    if (!db_) return;
    const char* sql = "UPDATE tasks SET result = ?, status = ? WHERE id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, result.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, task_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<Task> Orchestrator::load_in_flight_jobs() {
    std::vector<Task> jobs;
    if (!db_) return jobs;
    const char* sql = "SELECT id, payload FROM jobs WHERE phase NOT IN ('done','failed')";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return jobs;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Task t;
        t.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* payload = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        t.input_json = payload ? payload : "";
        jobs.push_back(std::move(t));
    }
    sqlite3_finalize(stmt);
    return jobs;
}

void Orchestrator::resume_in_flight() {
    if (!db_) return;
    const char* sql = "SELECT id, plan FROM jobs WHERE phase NOT IN ('done','failed')";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[orchestrator] resume query error: {}", sqlite3_errmsg(db_));
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string job_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* plan_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (!plan_cstr) {
            spdlog::warn("[orchestrator] resume job {} has no plan, skipping", job_id);
            continue;
        }
        std::string plan_json(plan_cstr);
        auto plan = parse_plan(job_id, plan_json);
        if (!plan) {
            spdlog::warn("[orchestrator] resume job {} failed to parse plan", job_id);
            continue;
        }
        spdlog::info("[orchestrator] resuming job {} ({} steps)", job_id, plan->steps.size());
        // Re-verify plan (should still be valid)
        auto verify = verifier_.verify(*plan);
        if (!verify.ok) {
            spdlog::error("[orchestrator] resume job {} plan verification failed, marking failed", job_id);
            update_job_phase(job_id, "failed");
            continue;
        }
        // Execute
        auto result = scheduler_.run(*plan);
        update_job_phase(job_id, result.success ? "done" : "failed");
    }
    sqlite3_finalize(stmt);
}

std::string Orchestrator::load_plan_json(const TaskId& job_id) {
    if (!db_) return "";
    const char* sql = "SELECT plan FROM jobs WHERE id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";
    sqlite3_bind_text(stmt, 1, job_id.c_str(), -1, SQLITE_TRANSIENT);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (txt) result = txt;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::string Orchestrator::serialize_plan(const Plan& plan) const {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("steps");
    w.StartArray();
    for (const auto& step : plan.steps) {
        w.StartObject();
        w.Key("id");      w.String(step.id.c_str());
        w.Key("command"); w.String(step.command.c_str());
        w.Key("args");
        w.StartObject();
        for (const auto& [k,v] : step.args) {
            w.Key(k.c_str());
            w.String(v.c_str());
        }
        w.EndObject();
        w.Key("depends_on");
        w.StartArray();
        for (const auto& d : step.depends_on) {
            w.String(d.c_str());
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return buf.GetString();
}

TaskResult Orchestrator::submit(const Task& task) {
    spdlog::info("[orchestrator] task submitted: {} goal='{}'", task.id, task.goal);

    // ── 0. Persist job (phase = planning) ──────────────────────────────────────
    if (db_) {
        store_job(task);
    }

    // ── 1. Find an adviser for this task ────────────────────────────────────────
    // For now we use "general" as the default domain.
    // TODO: extract domain from task metadata or route by goal keyword
    auto adviser = registry_.find_adviser("general");
    if (!adviser) {
        spdlog::error("[orchestrator] no adviser registered for domain 'general'");
        if (db_) update_job_phase(task.id, "failed");
        return { task.id, false, "", "no adviser available" };
    }
    spdlog::info("[orchestrator] routing to adviser: {} ({})", adviser->name, adviser->id);

    // ── 2. Ask the adviser for a plan ────────────────────────────────────────────
    auto plan = request_plan(*adviser, task);
    if (!plan) {
        if (db_) update_job_phase(task.id, "failed");
        return { task.id, false, "", "adviser failed to produce a plan" };
    }
    spdlog::info("[orchestrator] plan received: {} steps", plan->steps.size());

    // ── 2b. Persist plan and tasks ──────────────────────────────────────────────
    if (db_) {
        std::string plan_json = serialize_plan(*plan);
        update_job_plan(task.id, plan_json);
        for (const auto& step : plan->steps) {
            store_task(task.id, step);
        }
    }

    // ── 3. Verify the plan ─────────────────────────────────────────────────────
    auto verify = verifier_.verify(*plan);
    if (!verify.ok) {
        std::string errs;
        for (const auto& e : verify.errors) errs += "\n  - " + e;
        spdlog::error("[orchestrator] plan verification failed:{}", errs);
        if (db_) update_job_phase(task.id, "failed");
        return { task.id, false, "", "invalid plan: " + errs };
    }

    // ── 4. Execute via scheduler ───────────────────────────────────────────────
    if (db_) update_job_phase(task.id, "executing");
    auto result = scheduler_.run(*plan);
    if (db_) update_job_phase(task.id, result.success ? "done" : "failed");
    return result;
}

std::optional<Plan> Orchestrator::request_plan(const RegisteredAdviser& adviser,
                                                const Task&            task) {
    // Build the planning request:
    // {
    //   "goal": "...",
    //   "input": {...},
    //   "available_commands": [ { name, description, input, output, limits }, ... ]
    // }
    // The adviser uses available_commands to know what workers exist and what they do.
    // It must respond with a Plan JSON.

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("goal");    w.String(task.goal.c_str());
    w.Key("input");   w.RawValue(task.input_json.empty() ? "{}" : task.input_json.c_str(),
                                  task.input_json.empty() ? 2    : task.input_json.size(),
                                  rapidjson::kObjectType);
    w.Key("available_commands");
    w.RawValue(build_command_context().c_str(), build_command_context().size(),
               rapidjson::kArrayType);
    w.EndObject();

    std::string plan_json;
    bool        got_response = false;

    // Use dispatcher to send request to adviser and receive response
    // For now, we use a simple synchronous approach via the dispatcher's
    // send_request method (which will be implemented per ADR-3).
    dispatcher_.send_request(
        adviser.id,
        "task.plan",
        buf.GetString(),
        [&](const std::string& result, const std::string& error) {
            if (!error.empty()) {
                spdlog::error("[orchestrator] adviser {} returned error: {}", adviser.id, error);
            } else {
                plan_json = result;
            }
            got_response = true;
        }
    );

    // TODO Phase 0: this needs to be async; stub blocks inline for now
    if (!got_response || plan_json.empty()) return std::nullopt;
    return parse_plan(task.id, plan_json);
}

std::string Orchestrator::build_command_context() const {
    // Serialise all registered CommandSchemas into a JSON array.
    // This is sent to the agent so it knows exactly what tools exist,
    // their descriptions, input/output shapes, and limits.
    auto schemas = registry_.all_command_schemas();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    for (const auto& cmd : schemas) {
        w.StartObject();
        w.Key("name");        w.String(cmd.name.c_str());
        w.Key("description"); w.String(cmd.description.c_str());
        w.Key("timeout_ms");  w.Int(cmd.limits.timeout_ms);
        // TODO: serialise full input/output schema
        w.EndObject();
    }
    w.EndArray();
    return buf.GetString();
}

std::optional<Plan> Orchestrator::parse_plan(const TaskId&      task_id,
                                              const std::string& plan_json) const {
    // Expected plan JSON from agent:
    // {
    //   "steps": [
    //     {
    //       "id": "step_1",
    //       "command": "web.search",
    //       "args": { "query": "quantum computing" },
    //       "depends_on": []
    //     },
    //     ...
    //   ]
    // }

    rapidjson::Document doc;
    if (doc.Parse(plan_json.c_str()).HasParseError()) {
        spdlog::error("[orchestrator] failed to parse plan JSON");
        return std::nullopt;
    }

    Plan plan;
    plan.task_id = task_id;

    if (!doc.HasMember("steps") || !doc["steps"].IsArray()) {
        spdlog::error("[orchestrator] plan JSON missing 'steps' array");
        return std::nullopt;
    }

    for (const auto& s : doc["steps"].GetArray()) {
        PlanStep step;
        if (s.HasMember("id"))      step.id      = s["id"].GetString();
        if (s.HasMember("command")) step.command  = s["command"].GetString();

        if (s.HasMember("args") && s["args"].IsObject()) {
            for (auto it = s["args"].MemberBegin(); it != s["args"].MemberEnd(); ++it)
                step.args[it->name.GetString()] = it->value.GetString();
        }

        if (s.HasMember("depends_on") && s["depends_on"].IsArray()) {
            for (const auto& d : s["depends_on"].GetArray())
                step.depends_on.push_back(d.GetString());
        }

        plan.steps.push_back(std::move(step));
    }

    return plan;
}

} // namespace agentos
