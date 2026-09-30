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
#include "agentos/config.h"
#include "agentos/database.h"
#include "agentos/home_init.h"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <cstdlib>
#include <pwd.h>
#include <sstream>
#include <unistd.h>
#include <vector>
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

  // ---------------------------------------------------------------------------
  // Suite grants (ADR-015 amendment 2026-09-30). Deployment-time approval of
  // the privileged grants a Suite's Workers declare. These commands write
  // agentos.db directly, like `key generate`: approval is a deployment
  // action of whoever operates this host, and no JSON-RPC method -- admin
  // or otherwise -- can grant or revoke it.
  // ---------------------------------------------------------------------------

  std::unique_ptr<agentos::Database> open_db ()
  {
    auto db = std::make_unique<agentos::Database> (
      (agentos::agentos_home () / "agentos.db").string ());
    if (!db->open ())
      agentos::cli::die (5, "cannot open agentos.db");
    return db;
  }

  std::string approver ()
  {
    const passwd *pw = getpwuid (getuid ());
    return std::string ("cli:") + (pw && pw->pw_name ? pw->pw_name : "unknown");
  }

  // "network,gpu" or several arguments -> validated, de-duplicated list.
  std::vector<std::string> parse_grants (const std::vector<std::string> &in)
  {
    std::vector<std::string> out;
    for (const auto &arg : in)
    {
      std::stringstream ss (arg);
      std::string g;
      while (std::getline (ss, g, ','))
      {
        if (g.empty ())
          continue;
        if (!agentos::Database::suite_grant_valid (g))
          agentos::cli::die (1, "unknown grant '" + g
                                  + "'; use network and/or gpu");
        if (std::find (out.begin (), out.end (), g) == out.end ())
          out.push_back (g);
      }
    }
    if (out.empty ())
      agentos::cli::die (1, "no grant given; use network and/or gpu");
    return out;
  }

  // What one Worker's manifest asks for: {"network", "gpu"} subset.
  std::vector<std::string> requested_grants (const std::string &manifest)
  {
    std::vector<std::string> out;
    rapidjson::Document d;
    if (d.Parse (manifest.c_str ()).HasParseError () || !d.IsObject ()
        || !d.HasMember ("requires") || !d["requires"].IsObject ())
      return out;
    const auto &r = d["requires"];
    for (const char *g : { "network", "gpu" })
      if (r.HasMember (g) && r[g].IsBool () && r[g].GetBool ())
        out.emplace_back (g);
    return out;
  }

  // Print approvals and, per Worker, what it requests and what is in force.
  // Returns the number of requested-but-unapproved grants.
  int print_suite_grants (agentos::Database &db, const std::string &suite_id)
  {
    using namespace agentos::cli::color;
    agentos::Config cfg;
    {
      std::string err;
      if (auto c = agentos::load_config (
            (agentos::agentos_home () / "config.toml").string (), err))
        cfg = *c;
    }
    const auto approved = db.load_suite_grants (suite_id);
    auto is_approved = [&] (const std::string &g)
    {
      return std::any_of (approved.begin (), approved.end (),
                          [&] (const auto &r) { return r.grant == g; });
    };
    std::cout << bold (suite_id) << "\n  approved:";
    if (approved.empty ())
      std::cout << " " << grey ("none");
    for (const auto &r : approved)
      std::cout << " " << r.grant << " (" << r.approved_by << ", "
                << agentos::cli::fmt::time_ago (r.approved_at) << ")";
    std::cout << "\n";

    int pending = 0;
    for (const auto &c : db.load_suite_components (suite_id))
    {
      if (c.component_type != "worker")
        continue;
      const auto manifest = db.load_agent_manifest (c.component_id);
      const auto req = requested_grants (manifest.value_or ("{}"));
      if (req.empty ())
        continue;
      std::cout << "  " << c.component_id << ":";
      for (const auto &g : req)
      {
        const auto &list = g == "network" ? cfg.trusted_workers.network_exempt
                                          : cfg.trusted_workers.gpu;
        const bool by_config
          = std::find (list.begin (), list.end (), c.component_id)
            != list.end ();
        std::cout << " " << g << "=";
        if (is_approved (g))
          std::cout << green ("approved");
        else if (by_config)
          std::cout << green ("config.toml");
        else
        {
          std::cout << yellow ("NOT APPROVED");
          ++pending;
        }
      }
      std::cout << "\n";
    }
    return pending;
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
    auto approve = std::make_shared<std::vector<std::string>> ();
    install->add_option ("--approve", *approve)
      ->delimiter (',')
      ->description ("Approve the Suite's Workers' network and/or gpu "
                     "grants (deployment action; e.g. --approve network,gpu)");
    install->callback (
      [timeout_ms, socket_path, json_flag, path, approve]
      {
        // Validate before installing, so a typo cannot leave a Suite
        // installed without the approval its operator meant to give.
        const auto grants = approve->empty ()
                              ? std::vector<std::string>{}
                              : parse_grants (*approve);
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
            auto db = open_db ();
            for (const auto &g : grants)
              if (!db->set_suite_grant (result["suite_id"].GetString (), g,
                                        approver ()))
                agentos::cli::die (5, "installed, but cannot record approval '"
                                        + g + "'");
            print_json (result);
            return;
          }
          const std::string sid = result["suite_id"].GetString ();
          std::cout << "installed: " << sid << " ("
                    << result["components_registered"].GetInt ()
                    << " components)\n";
          auto db = open_db ();
          for (const auto &g : grants)
            if (!db->set_suite_grant (sid, g, approver ()))
              agentos::cli::die (5, "installed, but cannot record approval '"
                                      + g + "'");
          if (print_suite_grants (*db, sid) > 0)
            std::cout << "  approve with: agentos suite approve " << sid
                      << " network,gpu\n";
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

  // ---- suite approve | unapprove | grants (CLI only, no RPC) ----
  {
    auto *ap = suite->add_subcommand (
      "approve", "Approve a Suite's network/gpu grants (deployment, CLI only)");
    auto ap_suite = std::make_shared<std::string> ();
    auto ap_grants = std::make_shared<std::vector<std::string>> ();
    ap->add_option ("suite_id", *ap_suite)->required ();
    ap->add_option ("grants", *ap_grants, "network and/or gpu")->required ();
    ap->callback (
      [ap_suite, ap_grants]
      {
        const auto grants = parse_grants (*ap_grants);
        auto db = open_db ();
        if (!db->load_installed_suite (*ap_suite))
          agentos::cli::die (1, "suite '" + *ap_suite + "' is not installed");
        for (const auto &g : grants)
          if (!db->set_suite_grant (*ap_suite, g, approver ()))
            agentos::cli::die (5, "cannot record approval '" + g + "'");
        print_suite_grants (*db, *ap_suite);
        std::cout << "takes effect from the next Worker run; no restart needed\n";
      });
    agentos::cli::add_completion (ap);

    auto *un = suite->add_subcommand (
      "unapprove", "Withdraw a Suite's network/gpu approval (CLI only)");
    auto un_suite = std::make_shared<std::string> ();
    auto un_grants = std::make_shared<std::vector<std::string>> ();
    un->add_option ("suite_id", *un_suite)->required ();
    un->add_option ("grants", *un_grants, "network and/or gpu")->required ();
    un->callback (
      [un_suite, un_grants]
      {
        const auto grants = parse_grants (*un_grants);
        auto db = open_db ();
        for (const auto &g : grants)
          if (!db->revoke_suite_grant (*un_suite, g))
            std::cout << *un_suite << ": " << g << " was not approved\n";
        print_suite_grants (*db, *un_suite);
      });
    agentos::cli::add_completion (un);

    auto *gr = suite->add_subcommand (
      "grants", "Show approved and pending network/gpu grants per Suite");
    auto gr_suite = std::make_shared<std::string> ();
    gr->add_option ("suite_id", *gr_suite);
    gr->callback (
      [gr_suite]
      {
        auto db = open_db ();
        if (!gr_suite->empty ())
        {
          if (!db->load_installed_suite (*gr_suite))
            agentos::cli::die (1, "suite '" + *gr_suite + "' is not installed");
          print_suite_grants (*db, *gr_suite);
          return;
        }
        const auto suites = db->load_installed_suites ();
        if (suites.empty ())
          std::cout << "no suites installed\n";
        for (const auto &su : suites)
          if (su.enabled)
            print_suite_grants (*db, su.suite_id);
      });
    agentos::cli::add_completion (gr);
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
