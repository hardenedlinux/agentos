#include "agentos/orchestrator.h"
#include <spdlog/spdlog.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace agentos {

Orchestrator::Orchestrator(Registry&    registry,
                           Verifier&    verifier,
                           Scheduler&   scheduler,
                           Dispatcher&  dispatcher)
    : registry_(registry)
    , verifier_(verifier)
    , scheduler_(scheduler)
    , dispatcher_(dispatcher)
{}

TaskResult Orchestrator::submit(const Task& task) {
    spdlog::info("[orchestrator] task submitted: {} goal='{}'", task.id, task.goal);

    // ── 1. Find an adviser for this task ────────────────────────────────────────
    // For now we use "general" as the default domain.
    // TODO: extract domain from task metadata or route by goal keyword
    auto adviser = registry_.find_adviser("general");
    if (!adviser) {
        spdlog::error("[orchestrator] no adviser registered for domain 'general'");
        return { task.id, false, "", "no adviser available" };
    }
    spdlog::info("[orchestrator] routing to adviser: {} ({})", adviser->name, adviser->id);

    // ── 2. Ask the adviser for a plan ────────────────────────────────────────────
    auto plan = request_plan(*adviser, task);
    if (!plan) {
        return { task.id, false, "", "adviser failed to produce a plan" };
    }
    spdlog::info("[orchestrator] plan received: {} steps", plan->steps.size());

    // ── 3. Verify the plan ─────────────────────────────────────────────────────
    auto verify = verifier_.verify(*plan);
    if (!verify.ok) {
        std::string errs;
        for (const auto& e : verify.errors) errs += "\n  - " + e;
        spdlog::error("[orchestrator] plan verification failed:{}", errs);
        return { task.id, false, "", "invalid plan: " + errs };
    }

    // ── 4. Execute via scheduler ───────────────────────────────────────────────
    return scheduler_.run(*plan);
}

std::optional<Plan> Orchestrator::request_plan(const RegisteredAdviser& adviser,
                                                const Task&            task) {
    // Build the planning request:
    // {
    //   "goal": "...",
    //   "input": {...},
    //   "available_commands": [ { name, description, input, output, limits }, ... ]
    // }
    // The adviser uses available_commands to know what executors exist and what they do.
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
