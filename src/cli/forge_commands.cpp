#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/cli_format.h"
#include "agentos/forge_params.h"
#include <CLI/CLI.hpp>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <algorithm>
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

void register_forge_commands(CLI::App& app) {
    auto* forge = app.add_subcommand("forge", "Inspect forge pipeline");
    forge->require_subcommand(1);

    auto timeout_ms  = std::make_shared<int>(5000);
    auto socket_path = std::make_shared<std::string>();
    auto json_flag   = std::make_shared<bool>(false);
    forge->add_option("--timeout", *timeout_ms)->default_val(5000);
    forge->add_option("--socket",  *socket_path);
    forge->add_flag("--json",      *json_flag);

    // ---- forge list ----
    {
        auto* flist = forge->add_subcommand("list", "List forge jobs");
        std::string phase;
        flist->add_option("--phase", phase);
        flist->callback([timeout_ms, socket_path, json_flag, phase] {
            try {
                agentos::cli::CliClient client(*timeout_ms);
                if (!socket_path->empty()) client.set_socket_path(*socket_path);
                auto params = agentos::cli::build_forge_list_params(phase);
                auto result = client.send("forge.list", std::move(params));
                if (*json_flag) {
                    print_json(result);
                } else {
                    using namespace agentos::cli::color;
                    namespace f = agentos::cli::fmt;
                    if (!result.HasMember("forge_jobs") || !result["forge_jobs"].IsArray()) {
                        std::cout << "No forge jobs.\n";
                        return;
                    }
                    const auto& jobs = result["forge_jobs"];
                    struct Row { std::string id, phase, attempt, updated, requirement; };
                    std::vector<Row> rows;
                    for (const auto& j : jobs.GetArray()) {
                        std::string id    = f::str(j, "id");
                        std::string ph    = f::str(j, "phase");
                        int att = (j.HasMember("attempt")      && j["attempt"].IsInt())      ? j["attempt"].GetInt()      : 0;
                        int max = (j.HasMember("max_attempts") && j["max_attempts"].IsInt()) ? j["max_attempts"].GetInt() : 0;
                        std::string attempt;
                        if (att > 0 && max > 0) attempt = std::to_string(att) + "/" + std::to_string(max);
                        else if (att > 0)        attempt = std::to_string(att);
                        else                     attempt = "-";
                        std::string updated = f::ts(j, "updated_at");
                        std::string req     = f::str(j, "requirement");
                        if (req.size() > 40) req = req.substr(0, 40) + "...";
                        rows.push_back({id, ph, attempt, updated, req});
                    }

                    size_t w_id = 2, w_phase = 5, w_attempt = 7, w_updated = 7, w_req = 11;
                    for (const auto& r : rows) {
                        w_id      = std::max(w_id,      r.id.size());
                        w_phase   = std::max(w_phase,   r.phase.size());
                        w_attempt = std::max(w_attempt, r.attempt.size());
                        w_updated = std::max(w_updated, r.updated.size());
                        w_req     = std::max(w_req,     r.requirement.size());
                    }

                    size_t total_w = w_id + w_phase + w_attempt + w_updated + w_req + 8;

                    auto phase_color = [&](const std::string& p) -> std::string {
                        if (p == "promoted")                           return green(p);
                        if (p == "rejected" || p == "human_review")   return red(p);
                        if (p == "drafting" || p == "reviewing")      return yellow(p);
                        return p;
                    };

                    std::cout << bold(f::col("ID",          w_id))
                              << bold(f::col("PHASE",       w_phase))
                              << bold(f::col("ATTEMPT",     w_attempt))
                              << bold(f::col("UPDATED",     w_updated))
                              << bold(f::col("REQUIREMENT", w_req)) << "\n";
                    std::cout << f::separator(total_w) << "\n";

                    for (const auto& r : rows) {
                        std::cout << f::col(r.id,      w_id)
                                  << f::col_colored(phase_color(r.phase), r.phase, w_phase)
                                  << f::col(r.attempt, w_attempt)
                                  << f::col(r.updated, w_updated)
                                  << r.requirement << "\n";
                    }
                }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(flist);
    }

    // ---- forge status ----
    {
        auto* fstatus = forge->add_subcommand("status", "Show forge job status");
        std::string forge_id;
        fstatus->add_option("forge_id", forge_id)->required();
        fstatus->callback([timeout_ms, socket_path, forge_id] {
            try {
                agentos::cli::CliClient client(*timeout_ms);
                if (!socket_path->empty()) client.set_socket_path(*socket_path);
                auto params = agentos::cli::build_forge_status_params(forge_id);
                auto result = client.send("forge.status", std::move(params));
                print_json(result);
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(fstatus);
    }

    agentos::cli::add_completion(forge);
}
