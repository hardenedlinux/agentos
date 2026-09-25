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
#include "agentos/database.h"
#include "agentos/home_init.h"
#include "agentos/job_params.h"
#include <CLI/CLI.hpp>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
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

  std::string format_unix (int64_t ts)
  {
    std::time_t t = static_cast<std::time_t> (ts);
    std::tm tm{};
    localtime_r (&t, &tm);
    std::ostringstream oss;
    oss << std::put_time (&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str ();
  }

  // Local, direct-to-SQLite accessor — same pattern as key_commands.cpp's
  // open_db(). Used only by `user facts events`, which deliberately does
  // NOT go through CliClient/RPC: user_fact_events has no RPC method at
  // all (see the comment on Database::load_user_fact_events for why), so
  // the only way to inspect it is local direct access, same trust model
  // as `key generate/list/revoke`.
  std::unique_ptr<agentos::Database> open_db ()
  {
    auto home = agentos::agentos_home ();
    auto db
      = std::make_unique<agentos::Database> ((home / "agentos.db").string ());
    if (!db->open ())
      agentos::cli::die (5, "cannot open agentos.db");
    return db;
  }

} // unnamed namespace

void register_user_commands (CLI::App &app)
{
  auto *user = app.add_subcommand ("user", "Manage users (admin)");
  user->require_subcommand (1);

  auto timeout_ms = std::make_shared<int> (5000);
  auto socket_path = std::make_shared<std::string> ();
  auto json_flag = std::make_shared<bool> (false);
  auto access_key = std::make_shared<std::string> ();
  user->add_option ("--timeout", *timeout_ms)->default_val (5000);
  user->add_option ("--socket", *socket_path);
  user->add_flag ("--json", *json_flag);
  user->add_option (
    "--key", *access_key,
    "Access key (64-char hex); defaults to first active key in DB");

  // ---- user register ----
  {
    auto *reg = user->add_subcommand ("register", "Register a new user");
    auto user_id = std::make_shared<std::string> ();
    reg->add_option ("user_id", *user_id)->required ();

    reg->callback (
      [timeout_ms, socket_path, json_flag, access_key, user_id]
      {
        try
        {
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          if (!access_key->empty ())
            client.set_access_key (*access_key);

          rapidjson::Document params (rapidjson::kObjectType);
          auto &alloc = params.GetAllocator ();
          params.AddMember ("user_id",
                            rapidjson::Value (user_id->c_str (), alloc), alloc);

          auto result = client.send ("user.register", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            std::cout << "user_id: " << result["user_id"].GetString ()
                      << "  created: "
                      << format_unix (result["created_at"].GetInt64 ()) << "\n";
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (reg);
  }

  // ---- user list ----
  {
    auto *list = user->add_subcommand ("list", "List registered users");
    auto limit = std::make_shared<int> (50);
    auto offset = std::make_shared<int> (0);
    auto flag_enabled = std::make_shared<bool> (false);
    auto flag_disabled = std::make_shared<bool> (false);
    list->add_option ("--limit", *limit)->default_val (50);
    list->add_option ("--offset", *offset)->default_val (0);
    list->add_flag ("--enabled", *flag_enabled, "Show only enabled users");
    list->add_flag ("--disabled", *flag_disabled, "Show only disabled users");

    list->callback (
      [timeout_ms, socket_path, json_flag, access_key, limit, offset,
       flag_enabled, flag_disabled]
      {
        try
        {
          std::optional<bool> filter;
          if (*flag_enabled && !*flag_disabled)
            filter = true;
          else if (*flag_disabled && !*flag_enabled)
            filter = false;

          rapidjson::Document params (rapidjson::kObjectType);
          auto &alloc = params.GetAllocator ();
          if (filter)
            params.AddMember ("enabled", rapidjson::Value (*filter), alloc);
          params.AddMember ("limit", *limit, alloc);
          params.AddMember ("offset", *offset, alloc);

          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          if (!access_key->empty ())
            client.set_access_key (*access_key);
          auto result = client.send ("user.list", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            using namespace agentos::cli::color;
            if (!result.HasMember ("users") || !result["users"].IsArray ())
            {
              std::cout << "No users.\n";
              return;
            }
            const auto &users = result["users"];
            // Column widths: ID=10, ENABLED=10, CREATED=19
            // Header: pad manually to avoid ANSI escape width issues
            std::cout << bold ("ID") << std::string (8, ' ') << bold ("ENABLED")
                      << std::string (3, ' ') << bold ("CREATED") << "\n";
            for (const auto &u : users.GetArray ())
            {
              std::string id = u["id"].GetString ();
              bool enabled = u["enabled"].GetBool ();
              int64_t created = u["created_at"].GetInt64 ();
              // Pad id to 10 chars, then colored yes/no + manual padding to 10
              std::string id_padded
                = id + std::string (std::max (0, 10 - (int)id.size ()), ' ');
              std::string enabled_str
                = enabled ? (green ("yes") + std::string (7, ' '))
                          : (red ("no") + std::string (8, ' '));
              std::cout << id_padded << enabled_str << format_unix (created)
                        << "\n";
            }
            if (result.HasMember ("total"))
            {
              std::cout << "\ntotal: " << result["total"].GetInt ()
                        << " users\n";
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

  // ---- user enable ----
  {
    auto *en = user->add_subcommand ("enable", "Enable a user");
    auto user_id = std::make_shared<std::string> ();
    en->add_option ("user_id", *user_id)->required ();
    en->callback (
      [timeout_ms, socket_path, json_flag, access_key, user_id]
      {
        try
        {
          rapidjson::Document params (rapidjson::kObjectType);
          auto &alloc = params.GetAllocator ();
          params.AddMember ("user_id",
                            rapidjson::Value (user_id->c_str (), alloc), alloc);
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          if (!access_key->empty ())
            client.set_access_key (*access_key);
          auto result = client.send ("user.enable", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            std::cout << "enabled: " << *user_id << "\n";
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (en);
  }

  // ---- user disable ----
  {
    auto *dis = user->add_subcommand ("disable", "Disable a user");
    auto user_id = std::make_shared<std::string> ();
    dis->add_option ("user_id", *user_id)->required ();
    dis->callback (
      [timeout_ms, socket_path, json_flag, access_key, user_id]
      {
        try
        {
          rapidjson::Document params (rapidjson::kObjectType);
          auto &alloc = params.GetAllocator ();
          params.AddMember ("user_id",
                            rapidjson::Value (user_id->c_str (), alloc), alloc);
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          if (!access_key->empty ())
            client.set_access_key (*access_key);
          auto result = client.send ("user.disable", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            std::cout << "disabled: " << *user_id << "\n";
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (dis);
  }

  // ---- user profile ----
  {
    auto *prof
      = user->add_subcommand ("profile", "Show user behavioural profile");
    auto user_id = std::make_shared<std::string> ();
    prof->add_option ("user_id", *user_id)->required ();
    prof->callback (
      [timeout_ms, socket_path, json_flag, access_key, user_id]
      {
        try
        {
          rapidjson::Document params (rapidjson::kObjectType);
          auto &alloc = params.GetAllocator ();
          params.AddMember ("user_id",
                            rapidjson::Value (user_id->c_str (), alloc), alloc);
          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          if (!access_key->empty ())
            client.set_access_key (*access_key);
          auto result = client.send ("user.profile", std::move (params));
          if (*json_flag)
          {
            print_json (result);
          }
          else
          {
            using namespace agentos::cli::color;
            std::cout << "User ID:    " << result["user_id"].GetString ()
                      << "\n";
            std::cout << "First seen: "
                      << format_unix (result["first_seen"].GetInt64 ()) << "\n";
            if (result["last_seen"].IsNull ())
              std::cout << "Last seen:  null (no jobs)\n";
            else
              std::cout << "Last seen:  "
                        << format_unix (result["last_seen"].GetInt64 ())
                        << "\n";

            std::cout << "Total jobs:      " << result["total_jobs"].GetInt ()
                      << "\n";
            std::cout << "Successful jobs: "
                      << result["successful_jobs"].GetInt () << "\n";
            std::cout << "Failed jobs:     " << result["failed_jobs"].GetInt ()
                      << "\n";
            if (result.HasMember ("connected_providers")
                && result["connected_providers"].IsArray ())
            {
              std::cout << "Connected providers: ";
              const auto &arr = result["connected_providers"];
              for (rapidjson::SizeType i = 0; i < arr.Size (); ++i)
              {
                if (i)
                  std::cout << ", ";
                std::cout << arr[i].GetString ();
              }
              std::cout << "\n";
            }
          }
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (prof);
  }

  // ---- user facts (ADR-034 / ADR-036) ----
  {
    auto *facts = user->add_subcommand (
      "facts", "Manage per-user personalization facts");
    facts->require_subcommand (1);

    // ---- user facts record ----
    {
      auto *record
        = facts->add_subcommand ("record", "Record a user fact event");
      auto fact_type = std::make_shared<std::string> ();
      auto fact_key = std::make_shared<std::string> ();
      auto payload_str = std::make_shared<std::string> ();
      auto signal = std::make_shared<double> (0.0);

      record
        ->add_option ("--fact-type", *fact_type,
                      "category_interest | card_reaction | risk_preference "
                      "| market_region")
        ->required ();
      record->add_option ("--fact-key", *fact_key)->required ();
      record
        ->add_option ("--payload", *payload_str,
                      "JSON object, e.g. '{\"note\":\"...\"}'")
        ->required ();
      // Only required server-side for decayed fact_types (all but
      // card_reaction) — the daemon rejects the write with -32602 if
      // missing there, and ignores it entirely for card_reaction. Whether
      // it was actually passed on the command line (vs left at its 0.0
      // default) is checked via record->count(), not the value itself,
      // since 0.0 is also a legitimate signal.
      record->add_option ("--signal", *signal,
                          "Raw signal, required for decayed fact_types");

      record->callback (
        [timeout_ms, socket_path, json_flag, access_key, fact_type, fact_key,
         payload_str, signal, record]
        {
          try
          {
            rapidjson::Document payload;
            if (payload.Parse (payload_str->c_str ()).HasParseError ()
                || !payload.IsObject ())
            {
              agentos::cli::die (1, "--payload must be a valid JSON object");
            }

            rapidjson::Document params (rapidjson::kObjectType);
            auto &alloc = params.GetAllocator ();
            params.AddMember (
              "fact_type", rapidjson::Value (fact_type->c_str (), alloc),
              alloc);
            params.AddMember (
              "fact_key", rapidjson::Value (fact_key->c_str (), alloc),
              alloc);
            rapidjson::Value payload_copy (payload, alloc);
            params.AddMember ("payload", payload_copy, alloc);
            if (record->count ("--signal") > 0)
              params.AddMember ("signal", *signal, alloc);

            agentos::cli::CliClient client (*timeout_ms);
            if (!socket_path->empty ())
              client.set_socket_path (*socket_path);
            if (!access_key->empty ())
              client.set_access_key (*access_key);

            auto result = client.send ("user.facts.record", std::move (params));
            if (*json_flag)
              print_json (result);
            else
              std::cout << "recorded: " << *fact_type << "/" << *fact_key
                        << "\n";
          }
          catch (const agentos::cli::CliError &e)
          {
            agentos::cli::die (2, e.what ());
          }
        });
      agentos::cli::add_completion (record);
    }

    // ---- user facts get ----
    {
      auto *get
        = facts->add_subcommand ("get", "Read recorded/derived user facts");
      auto fact_types = std::make_shared<std::vector<std::string>> ();
      get->add_option ("--fact-type", *fact_types,
                       "Repeatable; omit to return all fact_types");

      get->callback (
        [timeout_ms, socket_path, json_flag, access_key, fact_types]
        {
          try
          {
            rapidjson::Document params (rapidjson::kObjectType);
            auto &alloc = params.GetAllocator ();
            if (!fact_types->empty ())
            {
              rapidjson::Value arr (rapidjson::kArrayType);
              for (const auto &ft : *fact_types)
                arr.PushBack (rapidjson::Value (ft.c_str (), alloc), alloc);
              params.AddMember ("fact_types", arr, alloc);
            }

            agentos::cli::CliClient client (*timeout_ms);
            if (!socket_path->empty ())
              client.set_socket_path (*socket_path);
            if (!access_key->empty ())
              client.set_access_key (*access_key);

            auto result = client.send ("user.facts.get", std::move (params));
            if (*json_flag)
            {
              print_json (result);
              return;
            }
            if (!result.HasMember ("facts") || !result["facts"].IsArray ())
            {
              std::cout << "No facts.\n";
              return;
            }
            for (const auto &f : result["facts"].GetArray ())
            {
              rapidjson::StringBuffer vb;
              rapidjson::Writer<rapidjson::StringBuffer> vw (vb);
              f["value"].Accept (vw);
              std::cout << f["fact_type"].GetString () << "  "
                        << f["fact_key"].GetString () << "  " << vb.GetString ()
                        << "  updated: "
                        << format_unix (f["updated_at"].GetInt64 ()) << "\n";
            }
          }
          catch (const agentos::cli::CliError &e)
          {
            agentos::cli::die (2, e.what ());
          }
        });
      agentos::cli::add_completion (get);
    }

    // ---- user facts events (local/debug only — bypasses RPC entirely) ----
    {
      auto *events = facts->add_subcommand (
        "events",
        "[local/debug] Dump raw user_fact_events rows directly from "
        "agentos.db — no RPC method exists for this by design (ADR-034: "
        "the event log is an internal audit trail, not a client-queryable "
        "surface). Must be run on the same machine as agentos.db.");
      auto ev_user_id = std::make_shared<std::string> ();
      auto ev_fact_type = std::make_shared<std::string> ();
      auto ev_limit = std::make_shared<int> (50);

      events->add_option ("--user-id", *ev_user_id,
                          "No authenticated-caller context exists for a "
                          "local command, so this must be given explicitly "
                          "— it's the access key id (first 8 hex chars of "
                          "the key hash), same value user.facts.* resolves "
                          "current_caller_key_id_ to server-side")
        ->required ();
      events->add_option ("--fact-type", *ev_fact_type);
      events->add_option ("--limit", *ev_limit)->default_val (50);

      events->callback (
        [json_flag, ev_user_id, ev_fact_type, ev_limit]
        {
          auto db = open_db ();
          std::optional<std::string> ft;
          if (!ev_fact_type->empty ())
            ft = *ev_fact_type;
          auto rows = db->load_user_fact_events (*ev_user_id, ft, *ev_limit);

          if (*json_flag)
          {
            rapidjson::Document doc (rapidjson::kObjectType);
            auto &alloc = doc.GetAllocator ();
            rapidjson::Value arr (rapidjson::kArrayType);
            for (const auto &r : rows)
            {
              rapidjson::Value o (rapidjson::kObjectType);
              o.AddMember ("id", r.id, alloc);
              o.AddMember ("fact_type",
                          rapidjson::Value (r.fact_type.c_str (), alloc), alloc);
              o.AddMember ("fact_key",
                          rapidjson::Value (r.fact_key.c_str (), alloc), alloc);
              rapidjson::Document payload_doc;
              if (!payload_doc.Parse (r.payload.c_str ()).HasParseError ())
              {
                rapidjson::Value payload_copy (payload_doc, alloc);
                o.AddMember ("payload", payload_copy, alloc);
              }
              else
                o.AddMember ("payload",
                            rapidjson::Value (r.payload.c_str (), alloc), alloc);
              o.AddMember ("source",
                          rapidjson::Value (r.source.c_str (), alloc), alloc);
              o.AddMember ("created_at", r.created_at, alloc);
              arr.PushBack (o, alloc);
            }
            doc.AddMember ("events", arr, alloc);
            print_json (doc);
          }
          else
          {
            if (rows.empty ())
            {
              std::cout << "No events.\n";
              return;
            }
            for (const auto &r : rows)
              std::cout << r.id << "  " << r.fact_type << "  " << r.fact_key
                        << "  " << r.payload << "  src=" << r.source << "  "
                        << format_unix (r.created_at) << "\n";
          }
        });
      agentos::cli::add_completion (events);
    }

    agentos::cli::add_completion (facts);
  }

  agentos::cli::add_completion (user);
}
