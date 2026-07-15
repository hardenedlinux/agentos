/**
 * Copyright (C) 2026  HardenedLinux community
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

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

namespace
{
  void print_json (const rapidjson::Document &doc)
  {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    doc.Accept (w);
    std::cout << buf.GetString () << "\n";
  }
} // namespace

void register_suite_commands (CLI::App &app)
{
  auto *suite
    = app.add_subcommand ("suite", "Manage Capability Suites (ADR-030)");
  suite->require_subcommand (1);

  auto timeout_ms = std::make_shared<int> (5000);
  auto socket_path = std::make_shared<std::string> ();
  auto json_flag = std::make_shared<bool> (false);
  suite->add_option ("--timeout", *timeout_ms)->default_val (5000);
  suite->add_option ("--socket", *socket_path);
  suite->add_flag ("--json", *json_flag);

  // ---- suite list ----
  {
    auto *list = suite->add_subcommand ("list", "List installed suites");
    list->callback (
                    [timeout_ms, socket_path, json_flag]
                    {
                      try
                        {
                          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          rapidjson::Document params (rapidjson::kObjectType);
          auto result = client.send ("suite.list", std::move (params));
          if (*json_flag)
          {
            print_json (result);
            return;
          }
          if (!result.HasMember ("suites") || !result["suites"].IsArray ()
              || result["suites"].Empty ())
          {
            std::cout << "No suites installed.\n";
            return;
          }
          for (const auto &s : result["suites"].GetArray ())
          {
            std::cout << s["suite_id"].GetString () << "\t"
                      << s["version"].GetString () << "\t"
                      << (s["enabled"].GetBool () ? "enabled" : "disabled")
                      << "\n";
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (list);
  }

  // ---- suite show ----
  {
    auto *show = suite->add_subcommand ("show", "Show suite details");
    auto suite_id = std::make_shared<std::string> ();
    show->add_option ("suite_id", *suite_id)->required ();
    show->callback (
      [timeout_ms, socket_path, json_flag, suite_id]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          rapidjson::Document params (rapidjson::kObjectType);
          auto &alloc = params.GetAllocator ();
          params.AddMember (
            "suite_id", rapidjson::Value (suite_id->c_str (), alloc), alloc);
          auto result = client.send ("suite.show", std::move (params));
          if (*json_flag)
          {
            print_json (result);
            return;
          }
          std::cout << "suite_id:  " << result["suite_id"].GetString () << "\n";
          std::cout << "version:   " << result["version"].GetString () << "\n";
          std::cout << "enabled:   "
                    << (result["enabled"].GetBool () ? "true" : "false")
                    << "\n";
          std::cout << "path:      " << result["install_path"].GetString ()
                    << "\n";
          std::cout << "components:\n";
          for (const auto &c : result["components"].GetArray ())
          {
            std::cout << "  [" << c["type"].GetString () << "] "
                      << c["id"].GetString ()
                      << (c["registered"].GetBool ()
                            ? ""
                            : "  (MISSING FROM REGISTRY)")
                      << "\n";
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (show);
  }

  // ---- suite install ----
  {
    auto *install = suite->add_subcommand (
      "install", "Install a suite from a local directory");
    auto path = std::make_shared<std::string> ();
    install->add_option ("--path", *path)
      ->required ()
      ->description (
        "Path to an unpacked Suite directory (containing suite.toml)");
    install->callback (
      [timeout_ms, socket_path, json_flag, path]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          rapidjson::Document params (rapidjson::kObjectType);
          auto &alloc = params.GetAllocator ();
          std::string abs_path = std::filesystem::absolute (*path).string ();
          params.AddMember ("path", rapidjson::Value (abs_path.c_str (), alloc),
                            alloc);
          auto result = client.send ("suite.install", std::move (params));
          if (*json_flag)
          {
            print_json (result);
            return;
          }
          std::cout << "installed: " << result["suite_id"].GetString () << " ("
                    << result["components_registered"].GetInt ()
                    << " components)\n";
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (install);
  }

  // ---- suite remove ----
  {
    auto *remove = suite->add_subcommand (
      "remove", "Remove (soft-revoke) an installed suite");
    auto suite_id = std::make_shared<std::string> ();
    remove->add_option ("suite_id", *suite_id)->required ();
    remove->callback (
      [timeout_ms, socket_path, json_flag, suite_id]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          rapidjson::Document params (rapidjson::kObjectType);
          auto &alloc = params.GetAllocator ();
          params.AddMember (
            "suite_id", rapidjson::Value (suite_id->c_str (), alloc), alloc);
          auto result = client.send ("suite.remove", std::move (params));
          if (*json_flag)
          {
            print_json (result);
            return;
          }
          std::cout << "removed: " << result["suite_id"].GetString () << " ("
                    << result["components_revoked"].GetInt ()
                    << " components revoked)\n";
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (remove);
  }

  suite->add_subcommand ("update", "Update a suite")
    ->callback (
      [] ()
      {
        std::cout << "suite update (not yet implemented — deferred, no "
                     "Marketplace source to update from yet)\n";
      });

  agentos::cli::add_completion (suite);
}
