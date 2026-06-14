#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/worker_params.h"
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

void register_worker_commands(CLI::App& app) {
    auto* worker = app.add_subcommand("worker", "Manage workers");
    worker->require_subcommand(1);

    int         timeout_ms  = 5000;
    std::string socket_path;
    bool        json_flag   = false;
    worker->add_option("--timeout", timeout_ms)->default_val(5000);
    worker->add_option("--socket",  socket_path);
    worker->add_flag("--json",      json_flag);

    // ---- worker register ----
    {
        auto* reg = worker->add_subcommand("register", "Register a worker");
        std::string path;
        reg->add_option("--path", path)->required();
        reg->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_worker_register_params(path);
                auto result = client.send("worker.register", std::move(params));
                if (json_flag) { print_json(result); }
                else { std::cout << "worker_id: " << result["worker_id"].GetString() << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(reg);
    }

    // ---- worker list ----
    {
        auto* list = worker->add_subcommand("list", "List workers");
        std::string enabled_str;
        list->add_option("--enabled", enabled_str);
        list->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_worker_list_params(enabled_str);
                auto result = client.send("worker.list", std::move(params));
                if (json_flag) {
                    print_json(result);
                } else {
                    if (result.HasMember("workers") && result["workers"].IsArray()) {
                        for (const auto& w : result["workers"].GetArray()) {
                            std::string caps;
                            if (w.HasMember("capabilities") && w["capabilities"].IsArray()) {
                                for (const auto& c : w["capabilities"].GetArray()) {
                                    if (!caps.empty()) caps += ",";
                                    caps += c.GetString();
                                }
                            }
                            std::cout
                                << (w.HasMember("id")      ? w["id"].GetString() : "")
                                << "  "
                                << (w.HasMember("tier")    ? w["tier"].GetString() : "")
                                << "  "
                                << (w.HasMember("enabled") ? (w["enabled"].GetBool() ? "true" : "false") : "")
                                << "  " << caps << "\n";
                        }
                    }
                }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(list);
    }

    // ---- worker enable ----
    {
        auto* enable = worker->add_subcommand("enable", "Enable a worker");
        std::string worker_id;
        enable->add_option("worker_id", worker_id)->required();
        enable->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_worker_toggle_params(worker_id);
                auto result = client.send("worker.enable", std::move(params));
                if (json_flag) { print_json(result); }
                else { std::cout << "enabled: " << worker_id << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(enable);
    }

    // ---- worker disable ----
    {
        auto* disable = worker->add_subcommand("disable", "Disable a worker");
        std::string worker_id;
        disable->add_option("worker_id", worker_id)->required();
        disable->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_worker_toggle_params(worker_id);
                auto result = client.send("worker.disable", std::move(params));
                if (json_flag) { print_json(result); }
                else { std::cout << "disabled: " << worker_id << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(disable);
    }

    agentos::cli::add_completion(worker);
}
