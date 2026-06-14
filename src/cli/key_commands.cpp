#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/database.h"
#include "agentos/home_init.h"
#include <CLI/CLI.hpp>
#include <openssl/sha.h>
#include <sys/random.h>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Crypto helpers
// ---------------------------------------------------------------------------

std::string base64url_encode(const uint8_t* data, size_t len)
{
    static const char alph[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    uint32_t val  = 0;
    int      valb = -6;
    for (size_t i = 0; i < len; ++i) {
        val  = (val << 8) | data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(alph[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
        out.push_back(alph[((val << 8) >> (valb + 8)) & 0x3F]);
    return out;
}

std::string sha256_hex(const uint8_t* data, size_t len)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, len, hash);
    std::ostringstream os;
    for (unsigned char c : hash)
        os << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);
    return os.str();
}

std::vector<uint8_t> random_bytes(size_t n)
{
    std::vector<uint8_t> buf(n);
    if (getrandom(buf.data(), buf.size(), 0) != static_cast<ssize_t>(n))
        agentos::cli::die(5, "getrandom failed");
    return buf;
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

int64_t now_unix()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::optional<int64_t> parse_expires(const std::string& s)
{
    if (s.empty() || s == "never") return std::nullopt;
    int64_t base = now_unix();
    if (s.size() >= 2) {
        char    unit = s.back();
        int64_t val  = std::stoll(s.substr(0, s.size() - 1));
        if (unit == 'd') return base + val * 86400LL;
        if (unit == 'y') return base + val * 365LL * 86400LL;
    }
    agentos::cli::die(1, "invalid --expires format; use e.g. 30d, 1y, or never");
}

std::string relative_time(int64_t unix_ts)
{
    int64_t diff = now_unix() - unix_ts;
    if (diff < 0)     return "future";
    if (diff < 60)    return std::to_string(diff) + "s ago";
    if (diff < 3600)  return std::to_string(diff / 60) + "min ago";
    if (diff < 86400) return std::to_string(diff / 3600) + "hr ago";
    return std::to_string(diff / 86400) + "d ago";
}

std::string format_expiry(const std::optional<int64_t>& ts)
{
    if (!ts) return "never";
    std::time_t t = static_cast<std::time_t>(*ts);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
}

// Pad string to width with spaces
std::string col(const std::string& s, size_t w)
{
    if (s.size() >= w) return s + "  ";
    return s + std::string(w - s.size(), ' ') + "  ";
}

std::unique_ptr<agentos::Database> open_db()
{
    auto home = agentos::agentos_home();
    auto db = std::make_unique<agentos::Database>((home / "agentos.db").string());
    if (!db->open())
        agentos::cli::die(5, "cannot open agentos.db");
    return db;
}

} // unnamed namespace

// ---------------------------------------------------------------------------
// register_key_commands
// ---------------------------------------------------------------------------

void register_key_commands(CLI::App& app)
{
    auto* key = app.add_subcommand("key", "Manage access keys");
    key->require_subcommand(1);

    // ---- key generate ----
    {
        auto* gen = key->add_subcommand("generate", "Generate a new access key");

        std::string role{"operator"};
        std::string description;
        std::string expires;

        gen->add_option("--role",        role)->default_val("operator");
        gen->add_option("--description", description);
        gen->add_option("--expires",     expires);

        gen->callback([&] {
            if (role != "admin" && role != "operator" && role != "readonly")
                agentos::cli::die(1, "invalid role; use admin, operator, or readonly");

            auto raw  = random_bytes(32);
            auto salt = random_bytes(16);

            std::string key_str  = base64url_encode(raw.data(),  raw.size());
            std::string salt_str = base64url_encode(salt.data(), salt.size());

            std::vector<uint8_t> concat;
            concat.insert(concat.end(), raw.begin(),  raw.end());
            concat.insert(concat.end(), salt.begin(), salt.end());
            std::string hash_str = sha256_hex(concat.data(), concat.size());

            auto db   = open_db();
            bool first = db->load_active_access_keys().empty();

            agentos::Database::AccessKey k;
            k.id          = hash_str.substr(0, 8);
            k.key         = key_str;
            k.key_hash    = hash_str;
            k.key_salt    = salt_str;
            k.description = description;
            k.role        = role;
            k.created_at  = now_unix();
            k.expires_at  = parse_expires(expires);

            db->insert_access_key(k);

            std::cout << "Generated access key (" << role << "):\n"
                      << "  ak_" << key_str << "\n";

            if (first) {
                std::cout
                    << "\n"
                    << "To connect a web bridge, generate an operator key:\n"
                    << "  agentos key generate --role operator --description \"web bridge\"\n"
                    << "\n"
                    << "Roles:\n"
                    << "  admin     Full access. Use for local CLI only.\n"
                    << "  operator  Submit jobs, review decisions. Use for bridges and connectors.\n"
                    << "  readonly  Query status only. Use for monitoring.\n";
            }
        });

        agentos::cli::add_completion(gen);
    }

    // ---- key list ----
    {
        auto* list = key->add_subcommand("list", "List all access keys");

        list->callback([&] {
            auto db   = open_db();
            auto keys = db->load_active_access_keys();

            if (keys.empty()) {
                std::cout << "No active keys. Run: agentos key generate\n";
                return;
            }

            // Compute column widths from content
            size_t w_id   = 8;
            size_t w_role = 8;
            size_t w_desc = 11;
            size_t w_exp  = 10;
            for (const auto& k : keys) {
                w_id   = std::max(w_id,   k.id.size());
                w_role = std::max(w_role, k.role.size());
                w_desc = std::max(w_desc, k.description.size());
            }

            size_t total_w = w_id + w_role + w_desc + w_exp + 9 + 8;

            using namespace agentos::cli::color;
            std::cout << bold(col("ID",          w_id))
                      << bold(col("ROLE",        w_role))
                      << bold(col("DESCRIPTION", w_desc))
                      << bold(col("EXPIRES",     w_exp))
                      << bold("LAST USED") << "\n"
                      << std::string(total_w, '-') << "\n";

            for (const auto& k : keys) {
                std::string role_colored;
                if      (k.role == "admin")    role_colored = yellow(k.role);
                else if (k.role == "operator") role_colored = green(k.role);
                else                           role_colored = cyan(k.role);

                std::string used = k.last_used_at
                    ? relative_time(*k.last_used_at)
                    : grey("never");

                std::cout << col(k.id,          w_id)
                          << col(role_colored,   w_role)
                          << col(k.description,  w_desc)
                          << col(format_expiry(k.expires_at), w_exp)
                          << used << "\n";
            }
        });

        agentos::cli::add_completion(list);
    }

    // ---- key revoke ----
    {
        auto* revoke = key->add_subcommand("revoke", "Revoke an access key");

        std::string id;
        std::string reason;

        revoke->add_option("id",       id)->required();
        revoke->add_option("--reason", reason);

        revoke->callback([&] {
            auto db = open_db();
            db->revoke_access_key(id, reason);
            std::cout << "revoked: " << id << "\n";
        });

        agentos::cli::add_completion(revoke);
    }

    agentos::cli::add_completion(key);
}
