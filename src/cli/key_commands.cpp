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

#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/database.h"
#include "agentos/home_init.h"
#include <CLI/CLI.hpp>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <openssl/sha.h>
#include <optional>
#include <sstream>
#include <string>
#include <sys/random.h>
#include <vector>

namespace
{

  // ---------------------------------------------------------------------------
  // Crypto helpers
  // ---------------------------------------------------------------------------

  // Encode raw bytes as lowercase hex string.
  // 32 bytes → 64 chars, 16 bytes → 32 chars.
  std::string hex_encode (const uint8_t *data, size_t len)
  {
    std::ostringstream os;
    for (size_t i = 0; i < len; ++i)
      os << std::hex << std::setfill ('0') << std::setw (2)
         << static_cast<int> (data[i]);
    return os.str ();
  }

  std::string hex_encode (const std::vector<uint8_t> &buf)
  {
    return hex_encode (buf.data (), buf.size ());
  }

  // SHA-256 of a string, returned as 64-char lowercase hex.
  // Used as: sha256_hex(key_hex + salt_hex) — both sides (generate and
  // orchestrator::authenticate) hash the hex-encoded strings, not raw bytes.
  std::string sha256_hex (const std::string &input)
  {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256 (reinterpret_cast<const unsigned char *> (input.data ()),
            input.size (), hash);
    return hex_encode (hash, SHA256_DIGEST_LENGTH);
  }

  std::vector<uint8_t> random_bytes (size_t n)
  {
    std::vector<uint8_t> buf (n);
    if (getrandom (buf.data (), buf.size (), 0) != static_cast<ssize_t> (n))
      agentos::cli::die (5, "getrandom failed");
    return buf;
  }

  // ---------------------------------------------------------------------------
  // Time helpers
  // ---------------------------------------------------------------------------

  int64_t now_unix ()
  {
    return static_cast<int64_t> (
      std::chrono::duration_cast<std::chrono::seconds> (
        std::chrono::system_clock::now ().time_since_epoch ())
        .count ());
  }

  std::optional<int64_t> parse_expires (const std::string &s)
  {
    if (s.empty () || s == "never")
      return std::nullopt;
    int64_t base = now_unix ();
    if (s.size () >= 2)
    {
      char unit = s.back ();
      int64_t val = std::stoll (s.substr (0, s.size () - 1));
      if (unit == 'd')
        return base + val * 86400LL;
      if (unit == 'y')
        return base + val * 365LL * 86400LL;
    }
    agentos::cli::die (1,
                       "invalid --expires format; use e.g. 30d, 1y, or never");
  }

  std::string relative_time (int64_t unix_ts)
  {
    int64_t diff = now_unix () - unix_ts;
    if (diff < 0)
      return "future";
    if (diff < 60)
      return std::to_string (diff) + "s ago";
    if (diff < 3600)
      return std::to_string (diff / 60) + "min ago";
    if (diff < 86400)
      return std::to_string (diff / 3600) + "hr ago";
    return std::to_string (diff / 86400) + "d ago";
  }

  std::string format_expiry (const std::optional<int64_t> &ts)
  {
    if (!ts)
      return "never";
    std::time_t t = static_cast<std::time_t> (*ts);
    std::tm tm{};
    gmtime_r (&t, &tm);
    char buf[16];
    std::strftime (buf, sizeof (buf), "%Y-%m-%d", &tm);
    return std::string (buf);
  }

  // Pad string to width with spaces
  std::string col (const std::string &s, size_t w)
  {
    if (s.size () >= w)
      return s + "  ";
    return s + std::string (w - s.size (), ' ') + "  ";
  }

  std::unique_ptr<agentos::Database> open_db ()
  {
    auto home = agentos::agentos_home ();
    auto db
      = std::make_unique<agentos::Database> ((home / "agentos.db").string ());
    if (!db->open ())
      agentos::cli::die (5, "cannot open agentos.db");
    return db;
  }

  // Colour role name consistently across generate/list output.
  std::string colour_role (const std::string &role)
  {
    using namespace agentos::cli::color;
    if (role == "admin")
      return yellow (role);
    if (role == "operator")
      return green (role);
    return cyan (role);
  }

} // unnamed namespace

// ---------------------------------------------------------------------------
// register_key_commands
//
// Option-bound variables are held in shared_ptr so that the CLI11 callbacks
// (which run during app.parse() in main) can safely read them after this
// function returns. Capturing locals by reference would leave the lambdas
// pointing at destroyed stack memory and produce arbitrary garbage at parse
// time (manifesting as e.g. "invalid role" regardless of --role value).
// ---------------------------------------------------------------------------

void register_key_commands (CLI::App &app)
{
  auto *key = app.add_subcommand ("key", "Manage access keys");
  key->require_subcommand (1);

  // ---- key generate ----
  {
    auto *gen = key->add_subcommand ("generate", "Generate a new access key");

    auto role = std::make_shared<std::string> ("operator");
    auto description = std::make_shared<std::string> ();
    auto expires = std::make_shared<std::string> ();

    gen->add_option ("--role", *role)->default_val ("operator");
    gen->add_option ("--description", *description);
    gen->add_option ("--expires", *expires);

    gen->callback (
      [role, description, expires]
      {
        if (*role != "admin" && *role != "operator" && *role != "readonly")
          agentos::cli::die (1,
                             "invalid role; use admin, operator, or readonly");

        auto raw = random_bytes (32);
        auto salt = random_bytes (16);

        // All fields stored and compared as lowercase hex:
        //   key      = hex(random 32 bytes)       → 64 chars
        //   key_salt = hex(random 16 bytes)       → 32 chars
        //   key_hash = SHA256(key + key_salt) hex → 64 chars
        //
        // Orchestrator::authenticate computes sha256_hex(key_value, ak.key_salt)
        // which is SHA256(key_hex + salt_hex) — identical to what we store here.
        std::string key_str = hex_encode (raw);
        std::string salt_str = hex_encode (salt);
        std::string hash_str = sha256_hex (key_str + salt_str);

        auto db = open_db ();
        bool first = db->load_active_access_keys ().empty ();

        agentos::Database::AccessKey k;
        k.id = hash_str.substr (0, 8);
        k.key = key_str;
        k.key_hash = hash_str;
        k.key_salt = salt_str;
        k.description = *description;
        k.role = *role;
        k.created_at = now_unix ();
        k.expires_at = parse_expires (*expires);

        db->insert_access_key (k);

        using namespace agentos::cli::color;

        // Header — what just happened
        std::cout << "\n"
                  << green ("✓") << " Generated access key ("
                  << colour_role (*role) << ")\n"
                  << "\n";

        // The key itself, indented and visually isolated for easy copy
        std::cout << "    " << bold ("ak_" + key_str) << "\n"
                  << "\n";

        // Security warning
        std::cout << "  " << yellow ("⚠")
                  << "  Save this key now — it will not be shown again.\n"
                  << "\n";

        // First-time guidance — suppressed on subsequent generations
        if (first)
        {
          std::cout
            << bold ("Next steps") << "\n"
            << "\n"
            << "  Generate an operator key for web bridges or connectors:\n"
            << "    "
            << grey ("agentos key generate --role operator --description "
                     "\"web bridge\"")
            << "\n"
            << "\n"
            << bold ("Roles") << "\n"
            << "\n"
            << "  " << yellow ("admin")
            << "     Full access. Use for local CLI only.\n"
            << "  " << green ("operator")
            << "  Submit jobs, review decisions. Use for bridges and "
               "connectors.\n"
            << "  " << cyan ("readonly")
            << "  Query status only. Use for monitoring.\n"
            << "\n";
        }
      });

    agentos::cli::add_completion (gen);
  }

  // ---- key list ----
  {
    auto *list = key->add_subcommand ("list", "List all access keys");

    list->callback (
      []
      {
        using namespace agentos::cli::color;
        auto db = open_db ();
        auto keys = db->load_active_access_keys ();

        if (keys.empty ())
        {
          std::cout << "\n"
                    << "  No active keys.\n"
                    << "\n"
                    << "  Run: " << grey ("agentos key generate") << "\n"
                    << "\n";
          return;
        }

        // Compute column widths from content
        size_t w_id = 8;
        size_t w_role = 8;
        size_t w_desc = 11;
        size_t w_exp = 10;
        for (const auto &k : keys)
        {
          w_id = std::max (w_id, k.id.size ());
          w_role = std::max (w_role, k.role.size ());
          w_desc = std::max (w_desc, k.description.size ());
        }

        size_t total_w = w_id + w_role + w_desc + w_exp + 9 + 8;

        std::cout << "\n"
                  << bold (col ("ID", w_id)) << bold (col ("ROLE", w_role))
                  << bold (col ("DESCRIPTION", w_desc))
                  << bold (col ("EXPIRES", w_exp)) << bold ("LAST USED")
                  << "\n"
                  << std::string (total_w, '-') << "\n";

        for (const auto &k : keys)
        {
          std::string used
            = k.last_used_at ? relative_time (*k.last_used_at) : grey ("never");

          std::cout << col (k.id, w_id) << col (colour_role (k.role), w_role)
                    << col (k.description, w_desc)
                    << col (format_expiry (k.expires_at), w_exp) << used
                    << "\n";
        }
        std::cout << "\n";
      });

    agentos::cli::add_completion (list);
  }

  // ---- key revoke ----
  {
    auto *revoke = key->add_subcommand ("revoke", "Revoke an access key");

    auto id = std::make_shared<std::string> ();
    auto reason = std::make_shared<std::string> ();

    revoke->add_option ("id", *id)->required ();
    revoke->add_option ("--reason", *reason);

    revoke->callback (
      [id, reason]
      {
        using namespace agentos::cli::color;
        auto db = open_db ();
        db->revoke_access_key (*id, *reason);
        std::cout << "\n"
                  << green ("✓") << " Revoked key " << bold (*id) << "\n"
                  << "\n";
      });

    agentos::cli::add_completion (revoke);
  }

  agentos::cli::add_completion (key);
}
