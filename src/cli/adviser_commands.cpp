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

#include "agentos/adviser_params.h"
#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/cli_format.h"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <iostream>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>
#include <vector>

namespace
{
  void print_json (const rapidjson::Document &doc)
  {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    doc.Accept (w);
    std::cout << buf.GetString () << "\n";
  }
} // unnamed namespace

void register_adviser_commands (CLI::App &app)
{
  auto *adviser = app.add_subcommand ("adviser", "Manage advisers");
  adviser->require_subcommand (1);

  auto timeout_ms = std::make_shared<int> (5000);
  auto socket_path = std::make_shared<std::string> ();
  auto json_flag = std::make_shared<bool> (false);
  adviser->add_option ("--timeout", *timeout_ms)->default_val (5000);
  adviser->add_option ("--socket", *socket_path);
  adviser->add_flag ("--json", *json_flag);

  // ---- adviser register ----
  {
    auto *reg = adviser->add_subcommand ("register", "Register an adviser");
    std::string path;
    reg->add_option ("--path", path)->required ();
    reg->callback (
      [timeout_ms, socket_path, json_flag, path]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          auto params = agentos::cli::build_adviser_register_params (path);
          auto result = client.send ("adviser.register", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            std::cout << "adviser_id: " << result["adviser_id"].GetString ()
                      << "\n";
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (reg);
  }

  // ---- adviser list ----
  {
    auto *list = adviser->add_subcommand ("list", "List advisers");
    list->callback (
      [timeout_ms, socket_path, json_flag]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          auto params = agentos::cli::build_adviser_list_params ();
          auto result = client.send ("adviser.list", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            using namespace agentos::cli::color;
            namespace f = agentos::cli::fmt;
            if (!result.HasMember ("advisers")
                || !result["advisers"].IsArray ())
            {
              std::cout << "No advisers.\n";
              return;
            }
            const auto &advisers = result["advisers"];
            struct Row
            {
              std::string id, model, active, desc;
            };
            std::vector<Row> rows;
            for (const auto &adv : advisers.GetArray ())
            {
              std::string id = f::str (adv, "id");
              std::string model = f::str (adv, "model");
              bool act = (adv.HasMember ("active") && adv["active"].IsBool ())
                           ? adv["active"].GetBool ()
                           : false;
              std::string active = act ? "true" : "false";
              std::string desc = f::str (adv, "description");
              rows.push_back ({id, model, active, desc});
            }

            size_t w_id = 2, w_model = 5, w_active = 6, w_desc = 11;
            for (const auto &r : rows)
            {
              w_id = std::max (w_id, r.id.size ());
              w_model = std::max (w_model, r.model.size ());
              w_active = std::max (w_active, r.active.size ());
              w_desc = std::max (w_desc, r.desc.size ());
            }

            size_t total_w = w_id + w_model + w_active + w_desc + 6;

            std::cout << bold (f::col ("ID", w_id))
                      << bold (f::col ("MODEL", w_model))
                      << bold (f::col ("ACTIVE", w_active))
                      << bold (f::col ("DESCRIPTION", w_desc)) << "\n";
            std::cout << f::separator (total_w) << "\n";

            for (const auto &r : rows)
            {
              std::string actColored
                = (r.active == "true") ? green ("true") : grey ("false");
              std::cout << f::col (r.id, w_id) << f::col (r.model, w_model)
                        << f::col_colored (actColored, r.active, w_active)
                        << r.desc << "\n";
            }
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (list);
  }

  agentos::cli::add_completion (adviser);
}
