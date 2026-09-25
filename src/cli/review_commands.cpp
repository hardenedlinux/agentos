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
#include "agentos/review_params.h"
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

void register_review_commands (CLI::App &app)
{
  auto *review = app.add_subcommand ("review", "Manage human review queue");
  review->require_subcommand (1);

  auto timeout_ms = std::make_shared<int> (5000);
  auto socket_path = std::make_shared<std::string> ();
  auto json_flag = std::make_shared<bool> (false);
  review->add_option ("--timeout", *timeout_ms)->default_val (5000);
  review->add_option ("--socket", *socket_path);
  review->add_flag ("--json", *json_flag);

  // ---- review list ----
  {
    auto *rlist = review->add_subcommand ("list", "List reviews");
    std::string status, type;
    rlist->add_option ("--status", status);
    rlist->add_option ("--type", type);
    rlist->callback (
      [timeout_ms, socket_path, json_flag, status, type]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          auto params = agentos::cli::build_review_list_params (status, type);
          auto result = client.send ("review.list", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            using namespace agentos::cli::color;
            namespace f = agentos::cli::fmt;
            if (!result.HasMember ("reviews") || !result["reviews"].IsArray ())
            {
              std::cout << "No reviews.\n";
              return;
            }
            const auto &reviews = result["reviews"];
            struct Row
            {
              std::string id, type, status, created, reason;
            };
            std::vector<Row> rows;
            for (const auto &rev : reviews.GetArray ())
            {
              std::string id = f::str (rev, "id");
              std::string type = f::str (rev, "type");
              std::string status = f::str (rev, "status");
              std::string reason = f::str (rev, "reason");
              if (reason.size () > 50)
                reason = reason.substr (0, 50) + "...";
              std::string created = f::ts (rev, "created_at");
              rows.push_back ({id, type, status, created, reason});
            }

            size_t w_id = 2, w_type = 4, w_status = 8, w_created = 7,
                   w_reason = 6;
            for (const auto &r : rows)
            {
              w_id = std::max (w_id, r.id.size ());
              w_type = std::max (w_type, r.type.size ());
              w_status = std::max (w_status, r.status.size ());
              w_created = std::max (w_created, r.created.size ());
              w_reason = std::max (w_reason, r.reason.size ());
            }

            size_t total_w
              = w_id + w_type + w_status + w_created + w_reason + 8;

            std::cout << bold (f::col ("ID", w_id))
                      << bold (f::col ("TYPE", w_type))
                      << bold (f::col ("STATUS", w_status))
                      << bold (f::col ("CREATED", w_created))
                      << bold (f::col ("REASON", w_reason)) << "\n";
            std::cout << f::separator (total_w) << "\n";

            for (const auto &r : rows)
            {
              std::string statusColored;
              if (r.status == "pending")
                statusColored = yellow (r.status);
              else if (r.status == "approved")
                statusColored = green (r.status);
              else if (r.status == "rejected")
                statusColored = red (r.status);
              else
                statusColored = r.status;
              std::cout << f::col (r.id, w_id) << f::col (r.type, w_type)
                        << f::col_colored (statusColored, r.status, w_status)
                        << f::col (r.created, w_created) << r.reason << "\n";
            }
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (rlist);
  }

  // ---- review show ----
  {
    auto *rshow = review->add_subcommand ("show", "Show review details");
    std::string review_id;
    rshow->add_option ("review_id", review_id)->required ();
    rshow->callback (
      [timeout_ms, socket_path, review_id]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          auto params = agentos::cli::build_review_id_params (review_id);
          auto result = client.send ("review.show", std::move (params));
          print_json (result);
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (rshow);
  }

  // ---- review approve ----
  {
    auto *rapprove = review->add_subcommand ("approve", "Approve a review");
    std::string review_id;
    rapprove->add_option ("review_id", review_id)->required ();
    rapprove->callback (
      [timeout_ms, socket_path, json_flag, review_id]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          auto params = agentos::cli::build_review_id_params (review_id);
          auto result = client.send ("review.approve", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            std::cout << "approved: " << review_id << "\n";
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (rapprove);
  }

  // ---- review reject ----
  {
    auto *rreject = review->add_subcommand ("reject", "Reject a review");
    std::string review_id, message;
    rreject->add_option ("review_id", review_id)->required ();
    rreject->add_option ("-m,--message", message)->required ();
    rreject->callback (
      [timeout_ms, socket_path, json_flag, review_id, message]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          auto params
            = agentos::cli::build_review_reject_params (review_id, message);
          auto result = client.send ("review.reject", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            std::cout << "rejected: " << review_id << "\n";
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (rreject);
  }

  agentos::cli::add_completion (review);
}
