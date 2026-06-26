#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/cli_format.h"
#include "agentos/job_params.h"
#include <CLI/CLI.hpp>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

void print_json(const rapidjson::Document& doc) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    doc.Accept(w);
    std::cout << buf.GetString() << "\n";
}

std::string format_unix(int64_t ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

} // unnamed namespace

void register_user_commands(CLI::App& app) {
    auto* user = app.add_subcommand("user", "Manage users (admin)");
    user->require_subcommand(1);

    auto timeout_ms  = std::make_shared<int>(5000);
    auto socket_path = std::make_shared<std::string>();
    auto json_flag   = std::make_shared<bool>(false);
    auto access_key  = std::make_shared<std::string>();
    user->add_option("--timeout", *timeout_ms)->default_val(5000);
    user->add_option("--socket",  *socket_path);
    user->add_flag("--json",      *json_flag);
    user->add_option("--key",     *access_key,
                     "Access key (64-char hex); defaults to first active key in DB");

    // ---- user register ----
    {
        auto* reg = user->add_subcommand("register", "Register a new user");
        auto user_id = std::make_shared<std::string>();
        reg->add_option("user_id", *user_id)->required();

        reg->callback([timeout_ms, socket_path, json_flag, access_key, user_id] {
            try {
                agentos::cli::CliClient client(*timeout_ms);
                if (!socket_path->empty()) client.set_socket_path(*socket_path);
                if (!access_key->empty()) client.set_access_key(*access_key);

                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                params.AddMember("user_id", rapidjson::Value(user_id->c_str(), alloc), alloc);

                auto result = client.send("user.register", std::move(params));
                if (*json_flag) { print_json(result); }
                else { std::cout << "user_id: "   << result["user_id"].GetString()
                                 << "  created: " << format_unix(result["created_at"].GetInt64())
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
        auto limit         = std::make_shared<int>(50);
        auto offset        = std::make_shared<int>(0);
        auto flag_enabled  = std::make_shared<bool>(false);
        auto flag_disabled = std::make_shared<bool>(false);
        list->add_option("--limit",  *limit)->default_val(50);
        list->add_option("--offset", *offset)->default_val(0);
        list->add_flag("--enabled",  *flag_enabled,  "Show only enabled users");
        list->add_flag("--disabled", *flag_disabled, "Show only disabled users");

        list->callback([timeout_ms, socket_path, json_flag, access_key, limit, offset, flag_enabled, flag_disabled] {
            try {
                std::optional<bool> filter;
                if (*flag_enabled && !*flag_disabled)
                    filter = true;
                else if (*flag_disabled && !*flag_enabled)
                    filter = false;

                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                if (filter) params.AddMember("enabled", rapidjson::Value(*filter), alloc);
                params.AddMember("limit",  *limit,  alloc);
                params.AddMember("offset", *offset, alloc);

                agentos::cli::CliClient client(*timeout_ms);
                if (!socket_path->empty()) client.set_socket_path(*socket_path);
                if (!access_key->empty()) client.set_access_key(*access_key);
                auto result = client.send("user.list", std::move(params));
                if (*json_flag) { print_json(result); }
                else {
                    using namespace agentos::cli::color;
                    if (!result.HasMember("users") || !result["users"].IsArray()) {
                        std::cout << "No users.\n";
                        return;
                    }
                    const auto& users = result["users"];
                    // Column widths: ID=10, ENABLED=10, CREATED=19
                    // Header: pad manually to avoid ANSI escape width issues
                    std::cout << bold("ID")      << std::string(8, ' ')
                              << bold("ENABLED") << std::string(3, ' ')
                              << bold("CREATED") << "\n";
                    for (const auto& u : users.GetArray()) {
                        std::string id = u["id"].GetString();
                        bool enabled = u["enabled"].GetBool();
                        int64_t created = u["created_at"].GetInt64();
                        // Pad id to 10 chars, then colored yes/no + manual padding to 10
                        std::string id_padded = id + std::string(std::max(0, 10 - (int)id.size()), ' ');
                        std::string enabled_str = enabled
                            ? (green("yes") + std::string(7, ' '))
                            : (red("no")    + std::string(8, ' '));
                        std::cout << id_padded << enabled_str << format_unix(created) << "\n";
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
        auto user_id = std::make_shared<std::string>();
        en->add_option("user_id", *user_id)->required();
        en->callback([timeout_ms, socket_path, json_flag, access_key, user_id] {
            try {
                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                params.AddMember("user_id", rapidjson::Value(user_id->c_str(), alloc), alloc);
                agentos::cli::CliClient client(*timeout_ms);
                if (!socket_path->empty()) client.set_socket_path(*socket_path);
                if (!access_key->empty()) client.set_access_key(*access_key);
                auto result = client.send("user.enable", std::move(params));
                if (*json_flag) { print_json(result); }
                else { std::cout << "enabled: " << *user_id << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(en);
    }

    // ---- user disable ----
    {
        auto* dis = user->add_subcommand("disable", "Disable a user");
        auto user_id = std::make_shared<std::string>();
        dis->add_option("user_id", *user_id)->required();
        dis->callback([timeout_ms, socket_path, json_flag, access_key, user_id] {
            try {
                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                params.AddMember("user_id", rapidjson::Value(user_id->c_str(), alloc), alloc);
                agentos::cli::CliClient client(*timeout_ms);
                if (!socket_path->empty()) client.set_socket_path(*socket_path);
                if (!access_key->empty()) client.set_access_key(*access_key);
                auto result = client.send("user.disable", std::move(params));
                if (*json_flag) { print_json(result); }
                else { std::cout << "disabled: " << *user_id << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(dis);
    }

    // ---- user profile ----
    {
        auto* prof = user->add_subcommand("profile", "Show user behavioural profile");
        auto user_id = std::make_shared<std::string>();
        prof->add_option("user_id", *user_id)->required();
        prof->callback([timeout_ms, socket_path, json_flag, access_key, user_id] {
            try {
                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                params.AddMember("user_id", rapidjson::Value(user_id->c_str(), alloc), alloc);
                agentos::cli::CliClient client(*timeout_ms);
                if (!socket_path->empty()) client.set_socket_path(*socket_path);
                if (!access_key->empty()) client.set_access_key(*access_key);
                auto result = client.send("user.profile", std::move(params));
                if (*json_flag) {
                    print_json(result);
                } else {
                    using namespace agentos::cli::color;
                    std::cout << "User ID:    " << result["user_id"].GetString() << "\n";
                    std::cout << "First seen: " << format_unix(result["first_seen"].GetInt64()) << "\n";
                    if (result["last_seen"].IsNull())
                        std::cout << "Last seen:  null (no jobs)\n";
                    else
                        std::cout << "Last seen:  " << format_unix(result["last_seen"].GetInt64()) << "\n";

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
