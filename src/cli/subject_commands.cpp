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

/**
 * agentos/src/cli/subject_commands.cpp
 *
 * CLI surface for ADR-035 (Subject Memory): subject.register,
 * subject.units.{populate,next,complete,progress},
 * subject.memory.{upsert,query}. Previously these methods existed only
 * in Orchestrator's dispatch table with no CLI entry point. Follows the
 * exact CliClient pattern used by job_commands.cpp / user_commands.cpp —
 * every subcommand builds a JSON-RPC params document and sends it
 * through the real Gateway socket, no direct library calls.
 */

#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include <CLI/CLI.hpp>
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

void register_subject_commands (CLI::App &app)
{
  auto *subject = app.add_subcommand ("subject", "Manage subjects (ADR-035)");
  subject->require_subcommand (1);

  // ---- shared options on `subject` (visible to every subcommand) ----
  auto timeout_ms = std::make_shared<int> (5000);
  auto socket_path = std::make_shared<std::string> ();
  auto json_flag = std::make_shared<bool> (false);
  auto access_key = std::make_shared<std::string> ();
  subject->add_option ("--timeout", *timeout_ms)->default_val (5000);
  subject->add_option ("--socket", *socket_path);
  subject->add_flag ("--json", *json_flag);
  subject->add_option (
    "--key", *access_key,
    "Access key (64-char hex); defaults to first active key in DB");

  // ---- subject register ----
  {
    auto *reg = subject->add_subcommand ("register", "Register a new subject");
    auto subject_type = std::make_shared<std::string> ();
    auto unit_type = std::make_shared<std::string> ();
    auto title = std::make_shared<std::string> ();
    reg->add_option ("--subject-type", *subject_type)->required ();
    reg->add_option ("--unit-type", *unit_type,
                     "Must be exactly \"file\" or \"line\" — the daemon "
                     "rejects any other value")
      ->required ();
    reg->add_option ("--title", *title);

    reg->callback (
      [timeout_ms, socket_path, json_flag, access_key, subject_type, unit_type,
       title]
      {
        try
        {
          rapidjson::Document params (rapidjson::kObjectType);
          auto &alloc = params.GetAllocator ();
          params.AddMember (
            "subject_type", rapidjson::Value (subject_type->c_str (), alloc),
            alloc);
          params.AddMember (
            "unit_type", rapidjson::Value (unit_type->c_str (), alloc), alloc);
          if (!title->empty ())
            params.AddMember ("title",
                              rapidjson::Value (title->c_str (), alloc),
                              alloc);

          agentos::cli::CliClient client (*timeout_ms);
          if (!socket_path->empty ())
            client.set_socket_path (*socket_path);
          if (!access_key->empty ())
            client.set_access_key (*access_key);

          auto result = client.send ("subject.register", std::move (params));
          if (*json_flag)
            print_json (result);
          else
            std::cout << "subject_id: " << result["subject_id"].GetString ()
                      << "\n";
        }
        catch (const agentos::cli::CliError &e)
        {
          agentos::cli::die (2, e.what ());
        }
      });
    agentos::cli::add_completion (reg);
  }

  // ---- subject units ---- (subgroup: populate / next / complete / progress)
  {
    auto *units = subject->add_subcommand ("units", "Manage subject units");
    units->require_subcommand (1);

    // ---- subject units populate ----
    {
      auto *populate
        = units->add_subcommand ("populate", "Register unit refs for a subject");
      auto subject_id = std::make_shared<std::string> ();
      auto unit_refs = std::make_shared<std::vector<std::string>> ();
      populate->add_option ("--subject-id", *subject_id)->required ();
      populate
        ->add_option ("--unit", *unit_refs,
                      "Repeatable; e.g. a file path or line-ref string")
        ->required ();

      populate->callback (
        [timeout_ms, socket_path, json_flag, access_key, subject_id, unit_refs]
        {
          try
          {
            rapidjson::Document params (rapidjson::kObjectType);
            auto &alloc = params.GetAllocator ();
            params.AddMember (
              "subject_id", rapidjson::Value (subject_id->c_str (), alloc),
              alloc);
            rapidjson::Value arr (rapidjson::kArrayType);
            for (const auto &u : *unit_refs)
              arr.PushBack (rapidjson::Value (u.c_str (), alloc), alloc);
            params.AddMember ("units", arr, alloc);

            agentos::cli::CliClient client (*timeout_ms);
            if (!socket_path->empty ())
              client.set_socket_path (*socket_path);
            if (!access_key->empty ())
              client.set_access_key (*access_key);

            auto result
              = client.send ("subject.units.populate", std::move (params));
            if (*json_flag)
              print_json (result);
            else
              std::cout << "inserted: " << result["inserted"].GetInt ()
                        << "  already_known: "
                        << result["already_known"].GetInt () << "\n";
          }
          catch (const agentos::cli::CliError &e)
          {
            agentos::cli::die (2, e.what ());
          }
        });
      agentos::cli::add_completion (populate);
    }

    // ---- subject units next ----
    {
      auto *next
        = units->add_subcommand ("next", "Fetch next pending units to process");
      auto subject_id = std::make_shared<std::string> ();
      auto limit = std::make_shared<int> (50);
      next->add_option ("--subject-id", *subject_id)->required ();
      next->add_option ("--limit", *limit)->default_val (50);

      next->callback (
        [timeout_ms, socket_path, json_flag, access_key, subject_id, limit]
        {
          try
          {
            rapidjson::Document params (rapidjson::kObjectType);
            auto &alloc = params.GetAllocator ();
            params.AddMember (
              "subject_id", rapidjson::Value (subject_id->c_str (), alloc),
              alloc);
            params.AddMember ("limit", *limit, alloc);

            agentos::cli::CliClient client (*timeout_ms);
            if (!socket_path->empty ())
              client.set_socket_path (*socket_path);
            if (!access_key->empty ())
              client.set_access_key (*access_key);

            auto result = client.send ("subject.units.next", std::move (params));
            if (*json_flag)
            {
              print_json (result);
              return;
            }
            if (!result.HasMember ("units") || !result["units"].IsArray ()
                || result["units"].Empty ())
            {
              std::cout << "No pending units.\n";
              return;
            }
            for (const auto &u : result["units"].GetArray ())
              std::cout << u["unit_index"].GetInt () << "  "
                        << u["unit_ref"].GetString () << "\n";
          }
          catch (const agentos::cli::CliError &e)
          {
            agentos::cli::die (2, e.what ());
          }
        });
      agentos::cli::add_completion (next);
    }

    // ---- subject units complete ----
    {
      auto *complete
        = units->add_subcommand ("complete", "Mark units as completed");
      auto subject_id = std::make_shared<std::string> ();
      auto indices = std::make_shared<std::vector<int>> ();
      complete->add_option ("--subject-id", *subject_id)->required ();
      complete
        ->add_option ("--index", *indices, "Repeatable; a unit_index from "
                                          "`subject units next`")
        ->required ();

      complete->callback (
        [timeout_ms, socket_path, json_flag, access_key, subject_id, indices]
        {
          try
          {
            rapidjson::Document params (rapidjson::kObjectType);
            auto &alloc = params.GetAllocator ();
            params.AddMember (
              "subject_id", rapidjson::Value (subject_id->c_str (), alloc),
              alloc);
            rapidjson::Value arr (rapidjson::kArrayType);
            for (int i : *indices)
              arr.PushBack (i, alloc);
            params.AddMember ("unit_indices", arr, alloc);

            agentos::cli::CliClient client (*timeout_ms);
            if (!socket_path->empty ())
              client.set_socket_path (*socket_path);
            if (!access_key->empty ())
              client.set_access_key (*access_key);

            auto result
              = client.send ("subject.units.complete", std::move (params));
            if (*json_flag)
              print_json (result);
            else
              std::cout << (result["ok"].GetBool () ? "ok\n" : "failed\n");
          }
          catch (const agentos::cli::CliError &e)
          {
            agentos::cli::die (2, e.what ());
          }
        });
      agentos::cli::add_completion (complete);
    }

    // ---- subject units progress ----
    {
      auto *progress
        = units->add_subcommand ("progress", "Show completion progress");
      auto subject_id = std::make_shared<std::string> ();
      progress->add_option ("--subject-id", *subject_id)->required ();

      progress->callback (
        [timeout_ms, socket_path, json_flag, access_key, subject_id]
        {
          try
          {
            rapidjson::Document params (rapidjson::kObjectType);
            auto &alloc = params.GetAllocator ();
            params.AddMember (
              "subject_id", rapidjson::Value (subject_id->c_str (), alloc),
              alloc);

            agentos::cli::CliClient client (*timeout_ms);
            if (!socket_path->empty ())
              client.set_socket_path (*socket_path);
            if (!access_key->empty ())
              client.set_access_key (*access_key);

            auto result
              = client.send ("subject.units.progress", std::move (params));
            if (*json_flag)
              print_json (result);
            else
              std::cout << result["completed"].GetInt () << " / "
                        << result["total"].GetInt () << " completed\n";
          }
          catch (const agentos::cli::CliError &e)
          {
            agentos::cli::die (2, e.what ());
          }
        });
      agentos::cli::add_completion (progress);
    }

    agentos::cli::add_completion (units);
  }

  // ---- subject memory ---- (subgroup: upsert / query)
  {
    auto *memory = subject->add_subcommand ("memory", "Manage subject memory entries");
    memory->require_subcommand (1);

    // ---- subject memory upsert ----
    {
      auto *upsert
        = memory->add_subcommand ("upsert", "Insert or update a memory entry");
      auto subject_id = std::make_shared<std::string> ();
      auto entry_key = std::make_shared<std::string> ();
      auto entry_value_str = std::make_shared<std::string> ();
      auto source_job_id = std::make_shared<std::string> ();
      auto asset_ids = std::make_shared<std::vector<std::string>> ();
      upsert->add_option ("--subject-id", *subject_id)->required ();
      upsert->add_option ("--entry-key", *entry_key)->required ();
      upsert
        ->add_option ("--entry-value", *entry_value_str,
                      "JSON object, e.g. '{\"summary\":\"...\"}'")
        ->required ();
      upsert->add_option ("--source-job-id", *source_job_id)->required ();
      upsert->add_option ("--asset-id", *asset_ids,
                         "Repeatable; related_asset_ids");

      upsert->callback (
        [timeout_ms, socket_path, json_flag, access_key, subject_id, entry_key,
         entry_value_str, source_job_id, asset_ids]
        {
          try
          {
            rapidjson::Document entry_value;
            if (entry_value.Parse (entry_value_str->c_str ()).HasParseError ()
                || !entry_value.IsObject ())
            {
              agentos::cli::die (1,
                                "--entry-value must be a valid JSON object");
            }

            rapidjson::Document params (rapidjson::kObjectType);
            auto &alloc = params.GetAllocator ();
            params.AddMember (
              "subject_id", rapidjson::Value (subject_id->c_str (), alloc),
              alloc);
            params.AddMember (
              "entry_key", rapidjson::Value (entry_key->c_str (), alloc),
              alloc);
            rapidjson::Value entry_value_copy (entry_value, alloc);
            params.AddMember ("entry_value", entry_value_copy, alloc);
            params.AddMember ("source_job_id",
                              rapidjson::Value (source_job_id->c_str (), alloc),
                              alloc);
            if (!asset_ids->empty ())
            {
              rapidjson::Value arr (rapidjson::kArrayType);
              for (const auto &a : *asset_ids)
                arr.PushBack (rapidjson::Value (a.c_str (), alloc), alloc);
              params.AddMember ("related_asset_ids", arr, alloc);
            }

            agentos::cli::CliClient client (*timeout_ms);
            if (!socket_path->empty ())
              client.set_socket_path (*socket_path);
            if (!access_key->empty ())
              client.set_access_key (*access_key);

            auto result
              = client.send ("subject.memory.upsert", std::move (params));
            if (*json_flag)
              print_json (result);
            else
              std::cout << "revision: " << result["revision"].GetInt ()
                        << "\n";
          }
          catch (const agentos::cli::CliError &e)
          {
            agentos::cli::die (2, e.what ());
          }
        });
      agentos::cli::add_completion (upsert);
    }

    // ---- subject memory query ----
    {
      auto *query
        = memory->add_subcommand ("query", "Query memory entries for a subject");
      auto subject_id = std::make_shared<std::string> ();
      auto key_prefix = std::make_shared<std::string> ();
      auto limit = std::make_shared<int> (100);
      auto cursor = std::make_shared<std::string> ();
      query->add_option ("--subject-id", *subject_id)->required ();
      query->add_option ("--key-prefix", *key_prefix);
      query->add_option ("--limit", *limit)->default_val (100);
      query->add_option ("--cursor", *cursor);

      query->callback (
        [timeout_ms, socket_path, json_flag, access_key, subject_id, key_prefix,
         limit, cursor]
        {
          try
          {
            rapidjson::Document params (rapidjson::kObjectType);
            auto &alloc = params.GetAllocator ();
            params.AddMember (
              "subject_id", rapidjson::Value (subject_id->c_str (), alloc),
              alloc);
            if (!key_prefix->empty ())
              params.AddMember (
                "key_prefix", rapidjson::Value (key_prefix->c_str (), alloc),
                alloc);
            params.AddMember ("limit", *limit, alloc);
            if (!cursor->empty ())
              params.AddMember ("cursor",
                                rapidjson::Value (cursor->c_str (), alloc),
                                alloc);

            agentos::cli::CliClient client (*timeout_ms);
            if (!socket_path->empty ())
              client.set_socket_path (*socket_path);
            if (!access_key->empty ())
              client.set_access_key (*access_key);

            auto result = client.send ("subject.memory.query", std::move (params));
            if (*json_flag)
            {
              print_json (result);
              return;
            }
            if (!result.HasMember ("entries") || !result["entries"].IsArray ()
                || result["entries"].Empty ())
            {
              std::cout << "No entries.\n";
              return;
            }
            for (const auto &e : result["entries"].GetArray ())
            {
              rapidjson::StringBuffer vb;
              rapidjson::Writer<rapidjson::StringBuffer> vw (vb);
              e["entry_value"].Accept (vw);
              std::cout << e["entry_key"].GetString () << "  rev "
                        << e["revision"].GetInt () << "  " << vb.GetString ()
                        << "\n";
            }
          }
          catch (const agentos::cli::CliError &e)
          {
            agentos::cli::die (2, e.what ());
          }
        });
      agentos::cli::add_completion (query);
    }

    agentos::cli::add_completion (memory);
  }

  agentos::cli::add_completion (subject);
}
