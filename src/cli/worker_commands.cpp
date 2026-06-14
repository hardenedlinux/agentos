#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/cli_format.h"
#include "agentos/worker_params.h"
#include <CLI/CLI.hpp>
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
                    using namespace agentos::cli::color;
                    namespace f = agentos::cli::fmt;
                    if (!result.HasMember("workers") || !result["workers"].IsArray()) {
                        std::cout << "No workers.\n";
                        return;
                    }
                    const auto& workers = result["workers"];
                    struct Row { std::string id, tier, prov, enabled, caps, registered; };
                    std::vector<Row> rows;
                    for (const auto& w : workers.GetArray()) {
                        std::string id   = f::str(w, "id");
                        std::string tier = f::str(w, "tier");
                        std::string prov = f::str(w, "provenance");
                        bool en = (w.HasMember("enabled") && w["enabled"].IsBool())
                                      ? w["enabled"].GetBool() : false;
                        std::string enabled = en ? "true" : "false";
                        std::string caps;
                        if (w.HasMember("capabilities") && w["capabilities"].IsArray()) {
                            for (const auto& c : w["capabilities"].GetArray()) {
                                if (!caps.empty()) caps += ",";
                                caps += c.GetString();
                            }
                        }
                        std::string registered = f::ts(w, "registered_at");
                        rows.push_back({id, tier, prov, enabled, caps, registered});
                    }

                    size_t w_id = 2, w_tier = 4, w_prov = 10, w_en = 7, w_caps = 13, w_reg = 10;
                    for (const auto& r : rows) {
                        w_id   = std::max(w_id,   r.id.size());
                        w_tier = std::max(w_tier, r.tier.size());
                        w_prov = std::max(w_prov, r.prov.size());
                        w_en   = std::max(w_en,   r.enabled.size());
                        w_caps = std::max(w_caps, r.caps.size());
                        w_reg  = std::max(w_reg,  r.registered.size());
                    }

                    size_t total_w = w_id + w_tier + w_prov + w_en + w_caps + w_reg + 10;

                    std::cout << bold(f::col("ID",           w_id))
                              << bold(f::col("TIER",         w_tier))
                              << bold(f::col("PROVENANCE",   w_prov))
                              << bold(f::col("ENABLED",      w_en))
                              << bold(f::col("CAPABILITIES", w_caps))
                              << bold(f::col("REGISTERED",   w_reg)) << "\n";
                    std::cout << f::separator(total_w) << "\n";

                    for (const auto& r : rows) {
                        std::string enColored = (r.enabled == "true") ? green("true") : red("false");
                        std::cout << f::col(r.id,   w_id)
                                  << f::col(r.tier, w_tier)
                                  << f::col(r.prov, w_prov)
                                  << f::col_colored(enColored, r.enabled, w_en)
                                  << f::col(r.caps, w_caps)
                                  << f::col(r.registered, w_reg) << "\n";
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
