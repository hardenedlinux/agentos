#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/cli_format.h"
#include "agentos/job_params.h"
#include <CLI/CLI.hpp>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_json(const rapidjson::Document& doc) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    doc.Accept(w);
    std::cout << buf.GetString() << "\n";
}

} // unnamed namespace

void register_job_commands(CLI::App& app) {
    auto* job = app.add_subcommand("job", "Manage jobs");
    job->require_subcommand(1);

    int         timeout_ms  = 5000;
    std::string socket_path;
    bool        json_flag   = false;
    job->add_option("--timeout", timeout_ms)->default_val(5000);
    job->add_option("--socket",  socket_path);
    job->add_flag("--json",      json_flag);

    // ---- job submit ----
    {
        auto* submit = job->add_subcommand("submit", "Submit a new job");

        std::string goal;
        std::string input_str;
        std::string type{"oneshot"};
        int64_t interval_s{0};
        int64_t starts_at{0};
        int max_iterations{5};
        std::string reviewer_id;
        std::string acceptance_criteria;

        submit->add_option("--goal", goal)->required();
        submit->add_option("--input", input_str);
        submit->add_option("--type", type)->default_val("oneshot");
        submit->add_option("--interval", interval_s);
        submit->add_option("--starts-at", starts_at);
        submit->add_option("--max-iterations", max_iterations)->default_val(5);
        submit->add_option("--reviewer", reviewer_id);
        submit->add_option("--acceptance-criteria", acceptance_criteria);

        submit->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_job_submit_params(
                    goal, type, input_str, interval_s, starts_at,
                    max_iterations, reviewer_id, acceptance_criteria);
                auto result = client.send("job.submit", std::move(params));
                if (json_flag) { print_json(result); }
                else { std::cout << "job_id: " << result["job_id"].GetString() << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(submit);
    }

    // ---- job status ----
    {
        auto* status = job->add_subcommand("status", "Get job status");
        std::string job_id;
        status->add_option("job_id", job_id)->required();

        status->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_job_status_params(job_id);
                auto result = client.send("job.status", std::move(params));
                if (json_flag) {
                    print_json(result);
                } else {
                    using namespace agentos::cli::color;
                    namespace f = agentos::cli::fmt;
                    if (!result.HasMember("job") || !result["job"].IsObject()) {
                        std::cerr << "invalid response\n";
                        return;
                    }
                    const auto& job = result["job"];

                    std::string id    = f::str(job, "id");
                    std::string type  = f::str(job, "type");
                    std::string goal  = f::str(job, "goal");
                    std::string phase = f::str(job, "phase");

                    std::string phaseColored;
                    if (phase == "executing" || phase == "repairing") phaseColored = yellow(phase);
                    else if (phase == "done")                         phaseColored = green(phase);
                    else if (phase == "failed" || phase == "human_review") phaseColored = red(phase);
                    else if (phase == "planning")                     phaseColored = cyan(phase);
                    else                                              phaseColored = phase;

                    std::cout << "Job " << id << "  [" << phaseColored << "]  " << type << "\n";
                    std::cout << "Goal: " << goal << "\n";
                    std::cout << "Created: " << f::ts(job, "created_at")
                              << "   Updated: " << f::ts(job, "updated_at") << "\n";

                    if (result.HasMember("steps") && result["steps"].IsArray()) {
                        std::cout << "\nSteps:\n";
                        for (const auto& step : result["steps"].GetArray()) {
                            int order = step.HasMember("step_order") ? step["step_order"].GetInt() : -1;
                            std::string desc       = f::str(step, "description");
                            std::string stepStatus = f::str(step, "status");
                            std::string started    = (step.HasMember("started_at")   && step["started_at"].IsInt64())
                                                       ? f::time_ago(step["started_at"].GetInt64())   : "-";
                            std::string completed  = (step.HasMember("completed_at") && step["completed_at"].IsInt64())
                                                       ? f::time_ago(step["completed_at"].GetInt64()) : "-";
                            std::string colStatus;
                            if      (stepStatus == "done")    colStatus = green(stepStatus);
                            else if (stepStatus == "running") colStatus = yellow(stepStatus);
                            else if (stepStatus == "failed")  colStatus = red(stepStatus);
                            else                              colStatus = grey(stepStatus);

                            std::cout << "  " << order << "  " << colStatus
                                      << "      " << desc
                                      << "          " << started << " \xe2\x86\x92 " << completed << "\n";
                        }
                    }

                    if (job.HasMember("loop") && job["loop"].IsObject()) {
                        const auto& loop = job["loop"];
                        int curIter = loop.HasMember("current_iteration") ? loop["current_iteration"].GetInt() : 0;
                        int maxIter = loop.HasMember("max_iterations")    ? loop["max_iterations"].GetInt()    : 0;
                        int repairs = loop.HasMember("current_repairs")   ? loop["current_repairs"].GetInt()   : 0;
                        std::cout << "Iteration: " << curIter << "/" << maxIter
                                  << "   Repairs: " << repairs << "\n";
                        if (loop.HasMember("last_feedback") && loop["last_feedback"].IsString()) {
                            std::cout << "Last feedback: " << loop["last_feedback"].GetString() << "\n";
                        }
                    }
                }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(status);
    }

    // ---- job list ----
    {
        auto* list = job->add_subcommand("list", "List jobs");
        std::string phase, type_filter;
        int limit{50}, offset{0};
        list->add_option("--phase", phase);
        list->add_option("--type", type_filter);
        list->add_option("--limit", limit)->default_val(50);
        list->add_option("--offset", offset)->default_val(0);

        list->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_job_list_params(phase, type_filter, limit, offset);
                auto result = client.send("job.list", std::move(params));
                if (json_flag) {
                    print_json(result);
                } else {
                    using namespace agentos::cli::color;
                    namespace f = agentos::cli::fmt;
                    if (!result.HasMember("jobs") || !result["jobs"].IsArray()) {
                        std::cout << "No jobs.\n";
                        return;
                    }
                    const auto& jobs = result["jobs"];
                    struct Row { std::string id, type, phase, goal, created; };
                    std::vector<Row> rows;
                    for (const auto& j : jobs.GetArray()) {
                        std::string id      = f::str(j, "id");
                        std::string type    = f::str(j, "type");
                        std::string phase   = f::str(j, "phase");
                        std::string goal    = f::str(j, "goal");
                        if (goal.size() > 40) goal = goal.substr(0, 40) + "...";
                        std::string created = f::ts(j, "created_at");
                        rows.push_back({id, type, phase, goal, created});
                    }

                    size_t w_id = 2, w_type = 4, w_phase = 5, w_goal = 5, w_created = 7;
                    for (const auto& r : rows) {
                        w_id      = std::max(w_id,      r.id.size());
                        w_type    = std::max(w_type,    r.type.size());
                        w_phase   = std::max(w_phase,   r.phase.size());
                        w_goal    = std::max(w_goal,    r.goal.size());
                        w_created = std::max(w_created, r.created.size());
                    }

                    size_t total_w = w_id + w_type + w_phase + w_goal + w_created + 8;

                    auto phase_color = [&](const std::string& p) -> std::string {
                        if (p == "executing" || p == "repairing") return yellow(p);
                        if (p == "done")                          return green(p);
                        if (p == "failed" || p == "human_review") return red(p);
                        if (p == "planning")                      return cyan(p);
                        return p;
                    };

                    std::cout << bold(f::col("ID",    w_id))
                              << bold(f::col("TYPE",  w_type))
                              << bold(f::col("PHASE", w_phase))
                              << bold(f::col("GOAL",  w_goal))
                              << bold("CREATED") << "\n";
                    std::cout << f::separator(total_w) << "\n";

                    for (const auto& r : rows) {
                        std::cout << f::col(r.id,   w_id)
                                  << f::col(r.type, w_type)
                                  << f::col_colored(phase_color(r.phase), r.phase, w_phase)
                                  << f::col(r.goal, w_goal)
                                  << r.created << "\n";
                    }

                    int total = (result.HasMember("total") && result["total"].IsInt())
                                    ? result["total"].GetInt()
                                    : static_cast<int>(rows.size());
                    std::cout << "\ntotal: " << total << " jobs\n";
                }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(list);
    }

    // ---- job cancel ----
    {
        auto* cancel = job->add_subcommand("cancel", "Cancel a job");
        std::string job_id;
        bool keep_schedule{false};
        cancel->add_option("job_id", job_id)->required();
        cancel->add_flag("--keep-schedule", keep_schedule);

        cancel->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_job_cancel_params(job_id, !keep_schedule);
                auto result = client.send("job.cancel", std::move(params));
                if (json_flag) { print_json(result); }
                else { std::cout << "cancelled: " << job_id << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(cancel);
    }

    agentos::cli::add_completion(job);
}
