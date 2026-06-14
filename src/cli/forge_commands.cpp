#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/forge_params.h"
#include <CLI/CLI.hpp>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <iostream>
#include <string>

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

    int         timeout_ms  = 5000;
    std::string socket_path;
    bool        json_flag   = false;
    forge->add_option("--timeout", timeout_ms)->default_val(5000);
    forge->add_option("--socket",  socket_path);
    forge->add_flag("--json",      json_flag);

    // ---- forge list ----
    {
        auto* flist = forge->add_subcommand("list", "List forge jobs");
        std::string phase;
        flist->add_option("--phase", phase);
        flist->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_forge_list_params(phase);
                auto result = client.send("forge.list", std::move(params));
                if (json_flag) {
                    print_json(result);
                } else {
                    if (result.HasMember("forge_jobs") && result["forge_jobs"].IsArray()) {
                        for (const auto& job : result["forge_jobs"].GetArray()) {
                            std::string attempt_str = job.HasMember("attempt") && job["attempt"].IsInt()
                                ? std::to_string(job["attempt"].GetInt()) : "?";
                            std::string max_str = job.HasMember("max_attempts") && job["max_attempts"].IsInt()
                                ? std::to_string(job["max_attempts"].GetInt()) : "?";
                            std::cout
                                << (job.HasMember("id")         ? job["id"].GetString()                             : "")
                                << "  "
                                << (job.HasMember("phase")      ? job["phase"].GetString()                          : "")
                                << "  attempt=" << attempt_str << "/" << max_str
                                << "  "
                                << (job.HasMember("updated_at") ? std::to_string(job["updated_at"].GetInt64())      : "")
                                << "\n";
                        }
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
        fstatus->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
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
