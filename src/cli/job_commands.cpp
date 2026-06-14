#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/job_params.h"
#include <CLI/CLI.hpp>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <iostream>
#include <string>
#include <cstdint>

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
                print_json(result);
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
                    if (result.HasMember("jobs") && result["jobs"].IsArray()) {
                        for (const auto& j : result["jobs"].GetArray()) {
                            std::cout
                                << (j.HasMember("id")         ? j["id"].GetString() : "")
                                << "  "
                                << (j.HasMember("type")       ? j["type"].GetString() : "")
                                << "  "
                                << (j.HasMember("phase")      ? j["phase"].GetString() : "")
                                << "  "
                                << (j.HasMember("created_at") ? std::to_string(j["created_at"].GetInt64()) : "")
                                << "\n";
                        }
                    }
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
