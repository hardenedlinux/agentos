#include "agentos/registry.h"
#include "agentos/database.h"
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace agentos
{

  struct Registry::Impl
  {
    sqlite3 *db = nullptr;

    // In-memory copies of the static catalog
    std::unordered_map<std::string, RegisteredAdviser> advisers; // key = id
    std::unordered_map<std::string, RegisteredExecutor> workers; // key = id
    // command -> worker id
    std::unordered_map<std::string, std::string> command_to_worker;
    // command -> schema
    std::unordered_map<std::string, CommandSchema> command_schemas;
  };

  // -----------------------------------------------------------------------
  // Helper: parse a JSON object into unordered_map<string, ArgSchema>
  // -----------------------------------------------------------------------
  static std::unordered_map<std::string, ArgSchema>
  parse_arg_schema (const rapidjson::Value &obj)
  {
    std::unordered_map<std::string, ArgSchema> result;
    if (!obj.IsObject ())
      return result;
    for (auto it = obj.MemberBegin (); it != obj.MemberEnd (); ++it)
    {
      ArgSchema arg;
      // The value can be a simple string (type) or an object with "type" and
      // optional "required" fields.
      if (it->value.IsString ())
      {
        arg.type = it->value.GetString ();
        arg.required = true;
      }
      else if (it->value.IsObject ())
      {
        const auto &val = it->value;
        if (val.HasMember ("type") && val["type"].IsString ())
          arg.type = val["type"].GetString ();
        else
          arg.type = "string";
        if (val.HasMember ("required") && val["required"].IsBool ())
          arg.required = val["required"].GetBool ();
        else
          arg.required = true;
        if (val.HasMember ("description") && val["description"].IsString ())
          arg.description = val["description"].GetString ();
      }
      else
      {
        // fallback
        arg.type = "string";
        arg.required = true;
      }
      result[it->name.GetString ()] = std::move (arg);
    }
    return result;
  }

  // -----------------------------------------------------------------------
  // Helper: parse a manifest JSON string into a RegisteredAdviser or
  // RegisteredExecutor
  // -----------------------------------------------------------------------
  static bool parse_manifest (const std::string &manifest_json,
                              const std::string &agent_id,
                              const std::string &role,
                              const std::string &binary_path,
                              RegisteredAdviser &out_adviser,
                              RegisteredExecutor &out_worker)
  {
    rapidjson::Document doc;
    doc.Parse (manifest_json.c_str ());
    if (doc.HasParseError () || !doc.IsObject ())
    {
      spdlog::error ("[registry] invalid manifest for agent '{}'", agent_id);
      return false;
    }

    // Common fields
    std::string name = agent_id;
    std::string version = "1.0";
    if (doc.HasMember ("name") && doc["name"].IsString ())
      name = doc["name"].GetString ();
    if (doc.HasMember ("version") && doc["version"].IsString ())
      version = doc["version"].GetString ();

    std::vector<std::string> domains;
    if (doc.HasMember ("domains") && doc["domains"].IsArray ())
    {
      for (const auto &d : doc["domains"].GetArray ())
        if (d.IsString ())
          domains.push_back (d.GetString ());
    }

    if (role == "adviser")
    {
      out_adviser.id = agent_id;
      out_adviser.name = name;
      out_adviser.version = version;
      out_adviser.binary_path = binary_path;
      out_adviser.domains = std::move (domains);
      return true;
    }

    // role == "worker"
    out_worker.id = agent_id;
    out_worker.name = name;
    out_worker.version = version;
    out_worker.binary_path = binary_path;

    if (!doc.HasMember ("capabilities") || !doc["capabilities"].IsArray ())
    {
      spdlog::warn ("[registry] worker '{}' has no capabilities", agent_id);
      return true; // still valid, just no commands
    }

    for (const auto &cap : doc["capabilities"].GetArray ())
    {
      if (!cap.IsObject ())
        continue;
      CommandSchema schema;
      if (cap.HasMember ("method") && cap["method"].IsString ())
        schema.name = cap["method"].GetString ();
      else
        continue; // skip entries without a method name

      if (cap.HasMember ("description") && cap["description"].IsString ())
        schema.description = cap["description"].GetString ();

      if (cap.HasMember ("input_schema") && cap["input_schema"].IsObject ())
        schema.input = parse_arg_schema (cap["input_schema"]);

      if (cap.HasMember ("output_schema") && cap["output_schema"].IsObject ())
        schema.output = parse_arg_schema (cap["output_schema"]);

      // resource_hints -> limits (optional)
      if (cap.HasMember ("resource_hints") && cap["resource_hints"].IsObject ())
      {
        const auto &hints = cap["resource_hints"];
        if (hints.HasMember ("timeout_ms") && hints["timeout_ms"].IsInt ())
          schema.limits.timeout_ms = hints["timeout_ms"].GetInt ();
        if (hints.HasMember ("max_input_len") && hints["max_input_len"].IsInt ())
          schema.limits.max_input_len = hints["max_input_len"].GetInt ();
      }

      out_worker.commands.push_back (std::move (schema));
    }
    return true;
  }

  // -----------------------------------------------------------------------
  // Registry implementation
  // -----------------------------------------------------------------------

  Registry::Registry ()
    : impl_ (std::make_unique<Impl> ())
  {
  }

  Registry::Registry (Database &db)
    : impl_ (std::make_unique<Impl> ())
  {
    load_from_db (db);
  }

  Registry::Registry (Registry &&other) noexcept
    : impl_ (std::move (other.impl_))
  {
  }

  Registry &Registry::operator= (Registry &&other) noexcept
  {
    if (this != &other)
      impl_ = std::move (other.impl_);
    return *this;
  }

  Registry::~Registry ()
  {
    // Do not close the database; it is owned by the Database object.
    impl_->db = nullptr;
  }

  void Registry::load_from_db (Database &db)
  {
    if (impl_->db)
    {
      spdlog::warn ("[registry] already loaded, ignoring");
      return;
    }

    impl_->db = db.db_handle ();
    if (!impl_->db)
    {
      spdlog::error ("[registry] database not open");
      return;
    }

    // Create tables if they don't exist (idempotent)
    const char *create_sql = R"(
      CREATE TABLE IF NOT EXISTS agents (
          id          TEXT PRIMARY KEY,
          role        TEXT NOT NULL,
          binary_path TEXT NOT NULL,
          manifest    TEXT NOT NULL,
          approved_by TEXT NOT NULL,
          approved_at INTEGER NOT NULL,
          enabled     INTEGER NOT NULL DEFAULT 1
      );
      CREATE TABLE IF NOT EXISTS capabilities (
          agent_id     TEXT NOT NULL REFERENCES agents(id),
          method       TEXT NOT NULL,
          description  TEXT NOT NULL,
          input_schema TEXT NOT NULL,
          cpu_weight   INTEGER,
          memory_mb    INTEGER,
          PRIMARY KEY (agent_id, method)
      );
    )";
    char *err = nullptr;
    int rc = sqlite3_exec (impl_->db, create_sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
      spdlog::error ("[registry] create tables: {}", err);
      sqlite3_free (err);
      return;
    }

    // Query enabled agents
    const char *query = R"(
      SELECT id, role, binary_path, manifest
      FROM agents
      WHERE enabled = 1
    )";
    sqlite3_stmt *stmt = nullptr;
    rc = sqlite3_prepare_v2 (impl_->db, query, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
      spdlog::error ("[registry] prepare query: {}", sqlite3_errmsg (impl_->db));
      return;
    }

    while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      std::string id = reinterpret_cast<const char *> (sqlite3_column_text (stmt, 0));
      std::string role = reinterpret_cast<const char *> (sqlite3_column_text (stmt, 1));
      std::string binary_path = reinterpret_cast<const char *> (sqlite3_column_text (stmt, 2));
      std::string manifest = reinterpret_cast<const char *> (sqlite3_column_text (stmt, 3));

      if (role == "adviser")
      {
        RegisteredAdviser adviser;
        RegisteredExecutor dummy;
        if (parse_manifest (manifest, id, role, binary_path, adviser, dummy))
        {
          impl_->advisers[id] = std::move (adviser);
        }
      }
      else if (role == "worker")
      {
        RegisteredAdviser dummy;
        RegisteredExecutor worker;
        if (parse_manifest (manifest, id, role, binary_path, dummy, worker))
        {
          impl_->workers[id] = worker;
          for (const auto &cmd : worker.commands)
          {
            impl_->command_to_worker[cmd.name] = id;
            impl_->command_schemas[cmd.name] = cmd;
          }
        }
      }
      else
      {
        spdlog::warn ("[registry] unknown role '{}' for agent '{}'", role, id);
      }
    }

    sqlite3_finalize (stmt);
    spdlog::info ("[registry] loaded {} advisers, {} workers",
                  impl_->advisers.size (), impl_->workers.size ());
  }

  void Registry::register_adviser (const RegisteredAdviser & /*adviser*/)
  {
    spdlog::warn ("[registry] register_adviser is deprecated (static catalog)");
  }

  void Registry::register_worker (const RegisteredExecutor & /*worker*/)
  {
    spdlog::warn ("[registry] register_worker is deprecated (static catalog)");
  }

  void Registry::remove (const ClientId & /*id*/)
  {
    spdlog::warn ("[registry] remove is deprecated (static catalog)");
  }

  std::optional<RegisteredAdviser>
  Registry::find_adviser (const std::string &domain) const
  {
    if (!impl_)
      return std::nullopt;
    // Return the first adviser whose domains list contains the requested
    // domain (or any adviser if domain is empty)
    for (const auto &[id, adviser] : impl_->advisers)
    {
      if (domain.empty ())
        return adviser;
      for (const auto &d : adviser.domains)
      {
        if (d == domain)
          return adviser;
      }
    }
    return std::nullopt;
  }

  std::optional<RegisteredExecutor>
  Registry::find_worker_for_command (const std::string &command) const
  {
    if (!impl_)
      return std::nullopt;
    auto it = impl_->command_to_worker.find (command);
    if (it == impl_->command_to_worker.end ())
      return std::nullopt;
    auto wit = impl_->workers.find (it->second);
    if (wit == impl_->workers.end ())
      return std::nullopt;
    return wit->second;
  }

  std::optional<CommandSchema>
  Registry::get_command_schema (const std::string &command) const
  {
    if (!impl_)
      return std::nullopt;
    auto it = impl_->command_schemas.find (command);
    if (it == impl_->command_schemas.end ())
      return std::nullopt;
    return it->second;
  }

  std::vector<CommandSchema> Registry::all_command_schemas () const
  {
    if (!impl_)
      return {};
    std::vector<CommandSchema> result;
    result.reserve (impl_->command_schemas.size ());
    for (const auto &[name, schema] : impl_->command_schemas)
      result.push_back (schema);
    return result;
  }

  size_t Registry::adviser_count () const
  {
    if (!impl_)
      return 0;
    return impl_->advisers.size ();
  }

  size_t Registry::worker_count () const
  {
    if (!impl_)
      return 0;
    return impl_->workers.size ();
  }

} // namespace agentos
