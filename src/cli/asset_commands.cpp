#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/cli_format.h"
#include <CLI/CLI.hpp>
#include <filesystem>
#include <iostream>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>

namespace {
void print_json(const rapidjson::Document& doc) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    doc.Accept(w);
    std::cout << buf.GetString() << "\n";
}
} // unnamed namespace

void register_asset_commands(CLI::App& app) {
    auto* asset = app.add_subcommand("asset", "Manage assets");
    asset->require_subcommand(1);

    auto timeout_ms  = std::make_shared<int>(5000);
    auto socket_path = std::make_shared<std::string>();
    auto json_flag   = std::make_shared<bool>(false);
    auto access_key  = std::make_shared<std::string>();
    asset->add_option("--timeout", *timeout_ms)->default_val(5000);
    asset->add_option("--socket",  *socket_path);
    asset->add_option("--key",     *access_key);
    asset->add_flag("--json",      *json_flag);

    // ---- asset register ----
    {
        auto* reg = asset->add_subcommand("register", "Register a local file as an asset");
        auto path = std::make_shared<std::string>();
        auto user_id = std::make_shared<std::string>("0");
        auto filename = std::make_shared<std::string>();
        reg->add_option("--path", *path)->required()
            ->description("Local absolute path to the file");
        reg->add_option("--user", *user_id)->default_val("0");
        reg->add_option("--filename", *filename)
            ->description("Display name to store (defaults to the path's basename)");
        reg->callback([timeout_ms, socket_path, json_flag, access_key, path, user_id, filename] {
            try {
                agentos::cli::CliClient client(*timeout_ms);
                if (!access_key->empty()) client.set_access_key(*access_key);
                if (!socket_path->empty()) client.set_socket_path(*socket_path);

                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                // Resolve here, in the CLI process — its cwd is the user's
                // actual shell directory. The daemon is a separate,
                // long-running process; a relative path sent as-is would
                // resolve against ITS working directory instead, silently
                // operating on the wrong file.
                std::string abs_path = std::filesystem::absolute(*path).string();
                params.AddMember("path", rapidjson::Value(abs_path.c_str(), alloc), alloc);
                params.AddMember("user_id", rapidjson::Value(user_id->c_str(), alloc), alloc);
                if (!filename->empty())
                    params.AddMember("filename", rapidjson::Value(filename->c_str(), alloc), alloc);

                auto result = client.send("asset.register", std::move(params));
                if (*json_flag) { print_json(result); }
                else {
                    std::cout << "asset_id: " << result["asset_id"].GetString() << "\n";
                    std::cout << "sha256:   " << result["sha256"].GetString() << "\n";
                    std::cout << "size:     " << result["size_bytes"].GetInt64() << " bytes\n";
                }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(reg);
    }

    // ---- asset show ----
    {
        auto* show = asset->add_subcommand("show", "Show asset details");
        auto asset_id = std::make_shared<std::string>();
        show->add_option("asset_id", *asset_id)->required();
        show->callback([timeout_ms, socket_path, json_flag, access_key, asset_id] {
            try {
                agentos::cli::CliClient client(*timeout_ms);
                if (!access_key->empty()) client.set_access_key(*access_key);
                if (!socket_path->empty()) client.set_socket_path(*socket_path);

                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                params.AddMember("asset_id", rapidjson::Value(asset_id->c_str(), alloc), alloc);

                auto result = client.send("asset.show", std::move(params));
                if (*json_flag) { print_json(result); }
                else {
                    std::cout << "asset_id:  " << result["asset_id"].GetString() << "\n";
                    std::cout << "user_id:   " << result["user_id"].GetString() << "\n";
                    std::cout << "filename:  " << result["original_filename"].GetString() << "\n";
                    std::cout << "sha256:    " << result["sha256"].GetString() << "\n";
                    std::cout << "size:      " << result["size_bytes"].GetInt64() << " bytes\n";
                    std::cout << "status:    " << result["status"].GetString() << "\n";
                    std::cout << "blob_path: " << result["blob_path"].GetString()
                              << (result["blob_exists"].GetBool() ? "" : "  (MISSING ON DISK)")
                              << "\n";
                }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(show);
    }

    // ---- asset revoke ----
    {
        auto* revoke = asset->add_subcommand("revoke", "Revoke (soft-delete) an asset");
        auto asset_id = std::make_shared<std::string>();
        revoke->add_option("asset_id", *asset_id)->required();
        revoke->callback([timeout_ms, socket_path, json_flag, access_key, asset_id] {
            try {
                agentos::cli::CliClient client(*timeout_ms);
                if (!access_key->empty()) client.set_access_key(*access_key);
                if (!socket_path->empty()) client.set_socket_path(*socket_path);

                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                params.AddMember("asset_id", rapidjson::Value(asset_id->c_str(), alloc), alloc);

                auto result = client.send("asset.revoke", std::move(params));
                if (*json_flag) { print_json(result); }
                else { std::cout << "revoked: " << result["asset_id"].GetString() << "\n"; }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(revoke);
    }

    // ---- asset extract ----
    {
        auto* extract = asset->add_subcommand("extract", "Extract an asset's content to a directory under its registered filename");
        auto asset_id = std::make_shared<std::string>();
        auto dest_dir = std::make_shared<std::string>();
        extract->add_option("asset_id", *asset_id)->required();
        extract->add_option("--dest", *dest_dir)->required()
            ->description("Directory to extract into (created if missing)");
        extract->callback([timeout_ms, socket_path, json_flag, access_key, asset_id, dest_dir] {
            try {
                agentos::cli::CliClient client(*timeout_ms);
                if (!access_key->empty()) client.set_access_key(*access_key);
                if (!socket_path->empty()) client.set_socket_path(*socket_path);

                rapidjson::Document params(rapidjson::kObjectType);
                auto& alloc = params.GetAllocator();
                params.AddMember("asset_id", rapidjson::Value(asset_id->c_str(), alloc), alloc);
                std::string abs_dest = std::filesystem::absolute(*dest_dir).string();
                params.AddMember("dest_dir", rapidjson::Value(abs_dest.c_str(), alloc), alloc);

                auto result = client.send("asset.extract", std::move(params));
                if (*json_flag) { print_json(result); }
                else {
                    std::cout << "extracted \"" << result["filename"].GetString()
                              << "\" to " << result["path"].GetString() << "\n";
                }
            } catch (const agentos::cli::CliError& e) {
                agentos::cli::die(2, e.what());
            }
        });
        agentos::cli::add_completion(extract);
    }

    agentos::cli::add_completion(asset);
}
