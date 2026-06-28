#include "agentos/registry.h"
#include "agentos/database.h"
#include "agentos/home_init.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <unordered_map>
#include <vector>

namespace agentos
{
  struct Registry::Impl
  {
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
      out_adviser.id = ClientId (agent_id);
      out_adviser.name = name;
      out_adviser.version = version;
      out_adviser.skill_path = binary_path;
      out_adviser.domains = std::move (domains);
      return true;
    }

    // role == "worker"
    out_worker.id = ClientId (agent_id);
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
        if (hints.HasMember ("max_input_len")
            && hints["max_input_len"].IsInt ())
          schema.limits.max_input_len = hints["max_input_len"].GetInt ();
      }

      out_worker.commands.push_back (std::move (schema));
    }
    return true;
  }

  // -----------------------------------------------------------------------
  // Registry implementation
  // -----------------------------------------------------------------------

  Registry::Registry () : impl_ (std::make_unique<Impl> ()) {}

  void Registry::init (Database &db)
  {
    impl_->advisers.clear ();
    impl_->workers.clear ();
    impl_->command_to_worker.clear ();
    impl_->command_schemas.clear ();

    for (const auto &row : db.load_enabled_agents ())
    {
      RegisteredAdviser adviser;
      RegisteredExecutor worker;
      if (!parse_manifest (row.manifest, row.id, row.role, row.binary_path,
                           adviser, worker))
        continue;

      if (row.role == "adviser")
        impl_->advisers[row.id] = std::move (adviser);
      else if (row.role == "worker")
      {
        impl_->workers[row.id] = worker;
        for (const auto &cmd : worker.commands)
        {
          impl_->command_to_worker[cmd.name] = row.id;
          impl_->command_schemas[cmd.name] = cmd;
        }
      }
    }

    // ADR-007: additional command -> agent entries from the capabilities
    // table. This covers agents whose manifest does not embed a
    // "capabilities" array (e.g. registered directly via insert_capability).
    // Entries here take precedence over manifest-derived ones for the same
    // method, but in practice the two are kept in sync (finalize_worker_promotion
    // writes both).
    for (const auto &cap : db.load_capabilities ())
    {
      CommandSchema schema;
      schema.name = cap.method;
      schema.description = cap.description;

      rapidjson::Document doc;
      doc.Parse (cap.input_schema.c_str ());
      if (!doc.HasParseError () && doc.IsObject ())
        schema.input = parse_arg_schema (doc);

      impl_->command_to_worker[cap.method] = cap.agent_id;
      impl_->command_schemas[cap.method] = schema;

      auto it = impl_->workers.find (cap.agent_id);
      if (it != impl_->workers.end ())
      {
        auto &cmds = it->second.commands;
        if (std::find_if (cmds.begin (), cmds.end (),
                          [&] (const CommandSchema &c)
                          { return c.name == schema.name; })
            == cmds.end ())
          cmds.push_back (schema);
      }
    }
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

  Registry::~Registry () {}

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

  // -----------------------------------------------------------------------
  // ADR-031: validate capability method format (namespace.verb)
  // Same rule as Database::insert_capability — kept in sync.
  // -----------------------------------------------------------------------
  static bool is_valid_method (const std::string &method)
  {
    if (method.empty () || method.size () > 64)
      return false;
    const auto dot = method.find ('.');
    if (dot == std::string::npos || dot != method.rfind ('.')
        || dot == 0 || dot + 1 == method.size ())
      return false;
    auto valid_segment = [] (const std::string &s, size_t start,
                              size_t len) -> bool
    {
      if (len == 0 || !(s[start] >= 'a' && s[start] <= 'z'))
        return false;
      for (size_t i = start + 1; i < start + len; ++i)
      {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
          return false;
      }
      return true;
    };
    return valid_segment (method, 0, dot)
           && valid_segment (method, dot + 1, method.size () - dot - 1);
  }

  // -----------------------------------------------------------------------
  // ADR-019: worker registration after forge pipeline promotes
  // -----------------------------------------------------------------------
  void Registry::finalize_worker_promotion (const ForgePipelineJob &job,
                                            const std::string &worker_code,
                                            const std::string &capability_json,
                                            Database &db)
  {
    auto home = agentos_home ();
    auto worker_dir = home / "workers" / job.id;
    std::error_code ec;
    std::filesystem::create_directories (worker_dir, ec);
    if (ec)
    {
      spdlog::error ("[registry] cannot create worker directory {}: {}",
                     worker_dir.string (), ec.message ());
      return;
    }

    // ADR-031: worker.py and worker_impl.py are written by
    // ForgeCoordinator::promote_worker before this call.
    // This function only writes manifest.json and handles DB/in-memory sync.
    auto code_path = worker_dir / "worker.py";

    // Write manifest.json
    auto manifest_path = worker_dir / "manifest.json";
    {
      std::ofstream out (manifest_path);
      if (!out)
      {
        spdlog::error ("[registry] cannot write manifest to {}",
                       manifest_path.string ());
        return;
      }
      out << capability_json;
    }

    // Insert agent record
    db.insert_agent (job.id, "worker", code_path.string (), capability_json);

    // Insert capability rows
    rapidjson::Document cap_doc;
    cap_doc.Parse (capability_json.c_str ());
    if (!cap_doc.HasParseError () && cap_doc.HasMember ("capabilities")
        && cap_doc["capabilities"].IsArray ())
    {
      for (const auto &cap : cap_doc["capabilities"].GetArray ())
      {
        if (!cap.IsObject ())
          continue;

        if (!cap.HasMember ("method") || !cap["method"].IsString ())
          continue;

        const std::string method = cap["method"].GetString ();

        // ADR-031: reject non-conforming method names before any write.
        // in-memory sync below uses the same guard, so DB and Registry
        // are always consistent.
        if (!is_valid_method (method))
        {
          spdlog::error (
            "[registry] finalize_worker_promotion: invalid method format '{}' "
            "for worker '{}' — skipping capability registration (ADR-031)",
            method, job.id);
          continue;
        }

        const std::string desc
          = cap.HasMember ("description") && cap["description"].IsString ()
              ? cap["description"].GetString ()
              : "";

        std::string input_schema = "{}";
        if (cap.HasMember ("input_schema") && cap["input_schema"].IsObject ())
        {
          rapidjson::StringBuffer ibuf;
          rapidjson::Writer<rapidjson::StringBuffer> iw (ibuf);
          cap["input_schema"].Accept (iw);
          input_schema = ibuf.GetString ();
        }

        db.insert_capability (job.id, method, desc, input_schema);
      }
    }
    else
    {
      spdlog::warn ("[registry] no capabilities in manifest for worker '{}'",
                    job.id);
    }

    // Update forge job status to promoted
    db.update_forge_pipeline_job_status (job.id, ForgeStatus::promoted);

    // Sync in-memory registry
    RegisteredExecutor executor;
    executor.id = ClientId (job.id);
    executor.name = job.id;
    executor.binary_path = code_path.string ();

    // Parse commands from capability_json
    if (!cap_doc.HasParseError () && cap_doc.HasMember ("capabilities")
        && cap_doc["capabilities"].IsArray ())
    {
      for (const auto &cap : cap_doc["capabilities"].GetArray ())
      {
        if (!cap.IsObject () || !cap.HasMember ("method")
            || !cap["method"].IsString ())
          continue;
        CommandSchema cmd;
        cmd.name = cap["method"].GetString ();
        // ADR-031: skip non-conforming method names — they were already
        // rejected in the DB insert pass above.
        if (!is_valid_method (cmd.name))
          continue;
        cmd.description
          = cap.HasMember ("description") && cap["description"].IsString ()
          ? cap["description"].GetString ()
          : "";
        executor.commands.push_back (std::move (cmd));
      }
    }

    for (const auto &cmd : executor.commands)
      {
        // ADR-031: only register methods that passed format validation above.
        // This prevents non-conforming names from entering the in-memory
        // Registry even when DB insert was already rejected.
        if (!is_valid_method (cmd.name))
        {
          spdlog::warn ("[registry] skipping in-memory registration of "
                        "invalid method '{}' for worker '{}'",
                        cmd.name, job.id);
          continue;
        }
        impl_->command_to_worker[cmd.name] = job.id;
        impl_->command_schemas[cmd.name] = cmd;
      }

    impl_->workers[job.id] = std::move (executor);

    spdlog::info ("[registry] worker '{}' promoted and registered", job.id);
  }

  std::optional<RegisteredAdviser>
  Registry::find_adviser_by_id (const std::string &id) const
  {
    if (!impl_)
      return std::nullopt;
    auto it = impl_->advisers.find (id);
    if (it == impl_->advisers.end ())
      return std::nullopt;
    return it->second;
  }

  std::vector<RegisteredAdviser> Registry::all_advisers () const
  {
    if (!impl_)
      return {};
    std::vector<RegisteredAdviser> result;
    result.reserve (impl_->advisers.size ());
    for (const auto &[id, adviser] : impl_->advisers)
      result.push_back (adviser);
    return result;
  }
} // namespace agentos
