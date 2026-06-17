#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/cli_format.h"
#include "agentos/job_params.h"
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

void register_user_commands(CLI::App& app) {
    auto* user = app.add_subcommand("user", "Manage users (admin)");
    user->require_subcommand(1);

    int         timeout_ms  = 5000;
    std::string socket_path;
    bool        json_flag   = false;
    user->add_option("--timeout", timeout_ms)->default_val(5000);
    user->add_option("--socket",  socket_path);
    user->add_flag("--json",      json_flag);

    // ---- user register ----
    {
        auto* reg = user->add_subcommand("register", "Register a new user");
        std::string user_id;
        reg->add_option("user_id", user_id)->required();

        reg->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);

                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                params.AddMember("user_id", rapidjson::Value(user_id.c_str(), alloc), alloc);

                auto result = client.send("user.register", std::move(params));
                if (json_flag) { print_json(result); }
                else { std::cout << "user_id: "   << result["user_id"].GetString()
                                 << "  created: " << result["created_at"].GetInt64()
                                 << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(reg);
    }

    // ---- user list ----
    {
        auto* list = user->add_subcommand("list", "List registered users");
        int limit{50}, offset{0};
        bool flag_enabled  = false;
        bool flag_disabled = false;
        list->add_option("--limit", limit)->default_val(50);
        list->add_option("--offset", offset)->default_val(0);
        list->add_flag("--enabled",  flag_enabled,  "Show only enabled users");
        list->add_flag("--disabled", flag_disabled, "Show only disabled users");

        list->callback([&] {
            try {
                std::optional<bool> filter;
                if (flag_enabled && !flag_disabled)
                    filter = true;
                else if (flag_disabled && !flag_enabled)
                    filter = false;

                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                if (filter) params.AddMember("enabled", rapidjson::Value(*filter), alloc);
                params.AddMember("limit",  limit,  alloc);
                params.AddMember("offset", offset, alloc);

                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto result = client.send("user.list", std::move(params));
                if (json_flag) { print_json(result); }
                else {
                    using namespace agentos::cli::color;
                    if (!result.HasMember("users") || !result["users"].IsArray()) {
                        std::cout << "No users.\n";
                        return;
                    }
                    const auto& users = result["users"];
                    std::cout << bold("ID") << "  " << bold("ENABLED")
                              << "  " << bold("CREATED") << "\n";
                    for (const auto& u : users.GetArray()) {
                        std::string id = u["id"].GetString();
                        bool enabled = u["enabled"].GetBool();
                        int64_t created = u["created_at"].GetInt64();
                        std::cout << id << "  "
                                  << (enabled ? green("yes") : red("no"))
                                  << "  " << created << "\n";
                    }
                    if (result.HasMember("total")) {
                        std::cout << "\ntotal: " << result["total"].GetInt()
                                  << " users\n";
                    }
                }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(list);
    }

    // ---- user enable ----
    {
        auto* en = user->add_subcommand("enable", "Enable a user");
        std::string user_id;
        en->add_option("user_id", user_id)->required();
        en->callback([&] {
            try {
                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                params.AddMember("user_id", rapidjson::Value(user_id.c_str(), alloc), alloc);
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto result = client.send("user.enable", std::move(params));
                if (json_flag) { print_json(result); }
                else { std::cout << "enabled: " << user_id << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(en);
    }

    // ---- user disable ----
    {
        auto* dis = user->add_subcommand("disable", "Disable a user");
        std::string user_id;
        dis->add_option("user_id", user_id)->required();
        dis->callback([&] {
            try {
                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                params.AddMember("user_id", rapidjson::Value(user_id.c_str(), alloc), alloc);
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto result = client.send("user.disable", std::move(params));
                if (json_flag) { print_json(result); }
                else { std::cout << "disabled: " << user_id << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(dis);
    }

    // ---- user profile ----
    {
        auto* prof = user->add_subcommand("profile", "Show user behavioural profile");
        std::string user_id;
        prof->add_option("user_id", user_id)->required();
        prof->callback([&] {
            try {
                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                params.AddMember("user_id", rapidjson::Value(user_id.c_str(), alloc), alloc);
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto result = client.send("user.profile", std::move(params));
                if (json_flag) {
                    print_json(result);
                } else {
                    using namespace agentos::cli::color;
                    auto print_nv = [&](const char* name, const rapidjson::Value& v, bool quote = false) {
                        if (v.IsInt64()) std::cout << name << ": " << v.GetInt64() << "\n";
                        else if (v.IsString()) {
                            std::cout << name << ": ";
                            if (quote) std::cout << "\"";
                            std::cout << v.GetString();
                            if (quote) std::cout << "\"";
                            std::cout << "\n";
                        } else if (v.IsNull()) std::cout << name << ": null\n";
                        else std::cout << name << ": " << v.GetInt() << "\n";
                    };
                    std::cout << "User ID:    " << result["user_id"].GetString() << "\n";
                    std::cout << "First seen: " << result["first_seen"].GetInt64() << "\n";
                    if (result["last_seen"].IsNull())
                        std::cout << "Last seen:  null (no jobs)\n";
                    else
                        std::cout << "Last seen:  " << result["last_seen"].GetInt64() << "\n";

                    std::cout << "Total jobs:      " << result["total_jobs"].GetInt() << "\n";
                    std::cout << "Successful jobs: " << result["successful_jobs"].GetInt() << "\n";
                    std::cout << "Failed jobs:     " << result["failed_jobs"].GetInt() << "\n";
                    if (result.HasMember("connected_providers") && result["connected_providers"].IsArray()) {
                        std::cout << "Connected providers: ";
                        const auto& arr = result["connected_providers"];
                        for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
                            if (i) std::cout << ", ";
                            std::cout << arr[i].GetString();
                        }
                        std::cout << "\n";
                    }
                }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(prof);
    }

    agentos::cli::add_completion(user);
}
