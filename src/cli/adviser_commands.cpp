#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/adviser_params.h"
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

void register_adviser_commands(CLI::App& app) {
    auto* adviser = app.add_subcommand("adviser", "Manage advisers");
    adviser->require_subcommand(1);

    int         timeout_ms  = 5000;
    std::string socket_path;
    bool        json_flag   = false;
    adviser->add_option("--timeout", timeout_ms)->default_val(5000);
    adviser->add_option("--socket",  socket_path);
    adviser->add_flag("--json",      json_flag);

    // ---- adviser register ----
    {
        auto* reg = adviser->add_subcommand("register", "Register an adviser");
        std::string path;
        reg->add_option("--path", path)->required();
        reg->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_adviser_register_params(path);
                auto result = client.send("adviser.register", std::move(params));
                if (json_flag) { print_json(result); }
                else { std::cout << "adviser_id: " << result["adviser_id"].GetString() << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(reg);
    }

    // ---- adviser list ----
    {
        auto* list = adviser->add_subcommand("list", "List advisers");
        list->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_adviser_list_params();
                auto result = client.send("adviser.list", std::move(params));
                if (json_flag) {
                    print_json(result);
                } else {
                    if (result.HasMember("advisers") && result["advisers"].IsArray()) {
                        for (const auto& adv : result["advisers"].GetArray()) {
                            std::cout
                                << (adv.HasMember("id")          ? adv["id"].GetString()          : "")
                                << "  "
                                << (adv.HasMember("description") ? adv["description"].GetString() : "")
                                << "  "
                                << (adv.HasMember("model")       ? adv["model"].GetString()       : "")
                                << "  active="
                                << (adv.HasMember("active") && adv["active"].IsBool()
                                        ? (adv["active"].GetBool() ? "true" : "false") : "")
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

    agentos::cli::add_completion(adviser);
}
