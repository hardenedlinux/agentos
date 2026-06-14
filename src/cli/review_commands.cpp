#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/review_params.h"
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

void register_review_commands(CLI::App& app) {
    auto* review = app.add_subcommand("review", "Manage human review queue");
    review->require_subcommand(1);

    int         timeout_ms  = 5000;
    std::string socket_path;
    bool        json_flag   = false;
    review->add_option("--timeout", timeout_ms)->default_val(5000);
    review->add_option("--socket",  socket_path);
    review->add_flag("--json",      json_flag);

    // ---- review list ----
    {
        auto* rlist = review->add_subcommand("list", "List reviews");
        std::string status, type;
        rlist->add_option("--status", status);
        rlist->add_option("--type", type);
        rlist->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_review_list_params(status, type);
                auto result = client.send("review.list", std::move(params));
                if (json_flag) {
                    print_json(result);
                } else {
                    if (result.HasMember("reviews") && result["reviews"].IsArray()) {
                        for (const auto& rev : result["reviews"].GetArray()) {
                            std::string reason;
                            if (rev.HasMember("reason") && rev["reason"].IsString()) {
                                reason = rev["reason"].GetString();
                                if (reason.size() > 60) reason = reason.substr(0, 60) + "...";
                            }
                            std::cout
                                << (rev.HasMember("id")     ? rev["id"].GetString()     : "")
                                << "  "
                                << (rev.HasMember("type")   ? rev["type"].GetString()   : "")
                                << "  "
                                << (rev.HasMember("status") ? rev["status"].GetString() : "")
                                << "  " << reason << "\n";
                        }
                    }
                }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(rlist);
    }

    // ---- review show ----
    {
        auto* rshow = review->add_subcommand("show", "Show review details");
        std::string review_id;
        rshow->add_option("review_id", review_id)->required();
        rshow->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_review_id_params(review_id);
                auto result = client.send("review.show", std::move(params));
                print_json(result);
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(rshow);
    }

    // ---- review approve ----
    {
        auto* rapprove = review->add_subcommand("approve", "Approve a review");
        std::string review_id;
        rapprove->add_option("review_id", review_id)->required();
        rapprove->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_review_id_params(review_id);
                auto result = client.send("review.approve", std::move(params));
                if (json_flag) { print_json(result); }
                else { std::cout << "approved: " << review_id << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(rapprove);
    }

    // ---- review reject ----
    {
        auto* rreject = review->add_subcommand("reject", "Reject a review");
        std::string review_id, message;
        rreject->add_option("review_id", review_id)->required();
        rreject->add_option("-m,--message", message)->required();
        rreject->callback([&] {
            try {
                agentos::cli::CliClient client(timeout_ms);
                if (!socket_path.empty()) client.set_socket_path(socket_path);
                auto params = agentos::cli::build_review_reject_params(review_id, message);
                auto result = client.send("review.reject", std::move(params));
                if (json_flag) { print_json(result); }
                else { std::cout << "rejected: " << review_id << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(rreject);
    }

    agentos::cli::add_completion(review);
}
