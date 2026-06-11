#include "agentos/verifier.h"
#include <unordered_set>
#include <spdlog/spdlog.h>

namespace agentos {

Verifier::Verifier(const Registry& registry) : registry_(registry) {}

VerifyResult Verifier::verify(const Plan& plan) const {
    VerifyResult result;
    result.ok = true;

    auto merge = [&](VerifyResult&& r) {
        if (!r.ok) {
            result.ok = false;
            result.errors.insert(result.errors.end(),
                                 r.errors.begin(), r.errors.end());
        }
    };

    merge(check_commands_exist(plan));
    merge(check_args(plan));
    merge(check_dag(plan));
    merge(check_variable_refs(plan));

    if (result.ok)
        spdlog::debug("[verifier] plan {} passed all checks ({} steps)",
            plan.task_id, plan.steps.size());
    else
        for (const auto& e : result.errors)
            spdlog::warn("[verifier] plan {} FAILED: {}", plan.task_id, e);

    return result;
}

VerifyResult Verifier::check_commands_exist(const Plan& plan) const {
    VerifyResult r; r.ok = true;
    for (const auto& step : plan.steps) {
        if (!registry_.get_command_schema(step.command)) {
            r.ok = false;
            r.errors.push_back("step '" + step.id + "': unknown command '" + step.command + "'");
        }
    }
    return r;
}

VerifyResult Verifier::check_args(const Plan& plan) const {
    VerifyResult r; r.ok = true;
    for (const auto& step : plan.steps) {
        auto schema = registry_.get_command_schema(step.command);
        if (!schema) continue;  // already flagged by check_commands_exist
        for (const auto& [arg_name, arg_schema] : schema->input) {
            if (arg_schema.required && step.args.find(arg_name) == step.args.end()) {
                r.ok = false;
                r.errors.push_back("step '" + step.id + "' command '" + step.command
                    + "': missing required arg '" + arg_name + "'");
            }
        }
    }
    return r;
}

VerifyResult Verifier::check_dag(const Plan& plan) const {
    // Kahn's algorithm: detect cycles in the depends_on graph
    VerifyResult r; r.ok = true;

    std::unordered_set<std::string> known_ids;
    for (const auto& step : plan.steps) known_ids.insert(step.id);

    // Check all depends_on references exist
    for (const auto& step : plan.steps) {
        for (const auto& dep : step.depends_on) {
            if (known_ids.find(dep) == known_ids.end()) {
                r.ok = false;
                r.errors.push_back("step '" + step.id
                    + "': depends_on unknown step '" + dep + "'");
            }
        }
    }

    // TODO Phase 1: full cycle detection via DFS
    return r;
}

VerifyResult Verifier::check_variable_refs(const Plan& plan) const {
    VerifyResult r; r.ok = true;
    std::unordered_set<std::string> known_ids;
    for (const auto& step : plan.steps) known_ids.insert(step.id);

    for (const auto& step : plan.steps) {
        for (const auto& [key, val] : step.args) {
            // Look for {{step_id.field}} references
            size_t pos = 0;
            while ((pos = val.find("{{", pos)) != std::string::npos) {
                size_t end = val.find("}}", pos);
                if (end == std::string::npos) break;
                std::string ref = val.substr(pos + 2, end - pos - 2);
                // ref is "step_id.field" — check step_id exists
                auto dot = ref.find('.');
                std::string ref_step = dot != std::string::npos ? ref.substr(0, dot) : ref;
                if (known_ids.find(ref_step) == known_ids.end()) {
                    r.ok = false;
                    r.errors.push_back("step '" + step.id + "' arg '" + key
                        + "': references unknown step '" + ref_step + "'");
                }
                pos = end + 2;
            }
        }
    }
    return r;
}

} // namespace agentos
