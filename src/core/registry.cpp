#include "agentos/registry.h"
#include "agentos/database.h"
#include "agentos/home_init.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <toml.hpp>
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
  // Helper: parse an Adviser's manifest.toml (ADR-018) into a
  // RegisteredAdviser. Advisers ship TOML on disk — this is a distinct
  // parser from parse_worker_manifest_json below, not a shared JSON path.
  // -----------------------------------------------------------------------
  static bool parse_adviser_manifest_toml (const std::string &manifest_toml,
                                           const std::string &agent_id,
                                           const std::string &binary_path,
                                           RegisteredAdviser &out_adviser)
  {
    toml::table tbl;
    try
    {
      tbl = toml::parse (manifest_toml);
    }
    catch (const toml::parse_error &e)
    {
      spdlog::error ("[registry] invalid manifest.toml for adviser '{}': {}",
                     agent_id, e.what ());
      return false;
    }

    auto meta = tbl["meta"];
    std::string name = meta["id"].value_or (agent_id);
    std::string version = meta["version"].value_or (std::string ("1.0"));
    std::string description = meta["description"].value_or (std::string ());

    std::vector<std::string> domains;
    if (auto domains_arr = meta["domains"].as_array ())
    {
      for (auto &&d : *domains_arr)
        if (auto s = d.value<std::string> ())
          domains.push_back (*s);
    }

    const int priority = meta["priority"].value_or (0);

    out_adviser.id = ClientId (agent_id);
    out_adviser.name = std::move (name);
    out_adviser.version = std::move (version);
    out_adviser.skill_path = binary_path;
    out_adviser.domains = std::move (domains);
    out_adviser.priority = priority;
    out_adviser.description = std::move (description);

    // ADR-038: read optional [continuation] section
    bool supports_cont = false;
    if (auto cont_node = tbl["continuation"]; cont_node.is_table ())
      supports_cont = cont_node["supports"].value_or (false);
    out_adviser.supports_continuation = supports_cont;

    // Optional [capabilities] allowed_advisers — absent entirely means
    // unrestricted (std::nullopt); present (even as an empty array) means
    // the orchestrator enforces this allow-list at Plan-validation time
    // (see RegisteredAdviser::allowed_advisers in types.h for why this
    // exists — a Plan-producing Adviser's own hallucination, not just a
    // malicious one, is the threat model here).
    if (auto caps_node = tbl["capabilities"]; caps_node.is_table ())
    {
      if (auto allowed_arr = caps_node["allowed_advisers"].as_array ())
      {
        std::vector<std::string> allowed;
        for (auto &&a : *allowed_arr)
          if (auto s = a.value<std::string> ())
            allowed.push_back (*s);
        out_adviser.allowed_advisers = std::move (allowed);
      }
      out_adviser.can_produce_plan
        = caps_node["can_produce_plan"].value_or (true);
    }

    return true;
  }

  // -----------------------------------------------------------------------
  // Helper: parse a Worker's manifest.json (ADR-031) into a
  // RegisteredExecutor. Workers ship JSON on disk — unchanged from before.
  // -----------------------------------------------------------------------
  static bool parse_worker_manifest_json (const std::string &manifest_json,
                                          const std::string &agent_id,
                                          const std::string &binary_path,
                                          RegisteredExecutor &out_worker)
  {
    rapidjson::Document doc;
    doc.Parse (manifest_json.c_str ());
    if (doc.HasParseError () || !doc.IsObject ())
    {
      spdlog::error ("[registry] invalid manifest.json for worker '{}'",
                     agent_id);
      return false;
    }

    std::string name = agent_id;
    std::string version = "1.0";
    if (doc.HasMember ("name") && doc["name"].IsString ())
      name = doc["name"].GetString ();
    if (doc.HasMember ("version") && doc["version"].IsString ())
      version = doc["version"].GetString ();

    out_worker.id = ClientId (agent_id);
    out_worker.name = name;
    out_worker.version = version;
    out_worker.binary_path = binary_path;

    // ADR-015/016 sandbox grants — top-level in manifest.json, worker-
    // instance-wide (not per-capability). May contain the reserved
    // placeholders __JOB_INPUT_PATH__/__JOB_OUTPUT_DIR__; substituting
    // those with a real per-job path is Orchestrator's job at dispatch
    // time, not Registry's — Registry has no job context here.
    // ADR-015/016 sandbox grants live under manifest.json's "requires"
    // object (CapabilityDeclaration shape — network/exec/fs_read/fs_write/
    // tcp_connect_ports), not at the top level — confirmed against the
    // actual Suite manifest, which nests them exactly this way.
    if (doc.HasMember ("requires") && doc["requires"].IsObject ())
    {
      const auto &req = doc["requires"];
      if (req.HasMember ("fs_read") && req["fs_read"].IsArray ())
        for (const auto &p : req["fs_read"].GetArray ())
          if (p.IsString ())
            out_worker.fs_read.push_back (p.GetString ());
      if (req.HasMember ("fs_write") && req["fs_write"].IsArray ())
        for (const auto &p : req["fs_write"].GetArray ())
          if (p.IsString ())
            out_worker.fs_write.push_back (p.GetString ());
      if (req.HasMember ("network") && req["network"].IsBool ())
        out_worker.network = req["network"].GetBool ();
    }

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
  // Dispatches to the correct format-specific parser by role. Kept as the
  // single call site Registry::init() uses, so callers don't need to know
  // that Advisers and Workers are stored in two different manifest formats.
  // -----------------------------------------------------------------------
  static bool parse_manifest (const std::string &manifest_text,
                              const std::string &agent_id,
                              const std::string &role,
                              const std::string &binary_path,
                              RegisteredAdviser &out_adviser,
                              RegisteredExecutor &out_worker)
  {
    if (role == "adviser")
      return parse_adviser_manifest_toml (manifest_text, agent_id, binary_path,
                                          out_adviser);
    return parse_worker_manifest_json (manifest_text, agent_id, binary_path,
                                       out_worker);
  }

  // -----------------------------------------------------------------------
  // Registry implementation
  // -----------------------------------------------------------------------

  Registry::Registry () : impl_ (std::make_unique<Impl> ()) {}

  // case‑insensitive comparison helper
  static bool iequals (const std::string &a, const std::string &b)
  {
    if (a.size () != b.size ())
      return false;
    for (std::size_t i = 0; i < a.size (); ++i)
      if (std::tolower (static_cast<unsigned char> (a[i]))
          != std::tolower (static_cast<unsigned char> (b[i])))
        return false;
    return true;
  }

  std::vector<RegisteredAdviser> Registry::find_advisers_by_domain (
    const std::vector<std::string> &goal_tokens) const
  {
    std::vector<RegisteredAdviser> result;
    if (!impl_)
      return result;

    for (const auto &[id, adv] : impl_->advisers)
    {
      bool match = false;
      for (const auto &domain : adv.domains)
      {
        // Split the domain tag on hyphens and compare each sub‑token
        // individually against the goal tokens (case‑insensitive).
        std::string sub;
        for (char c : domain)
        {
          if (c == '-')
          {
            if (!sub.empty ())
            {
              for (const auto &token : goal_tokens)
                if (iequals (sub, token))
                {
                  match = true;
                  break;
                }
              sub.clear ();
              if (match)
                break;
            }
          }
          else
            sub += static_cast<char> (
              std::tolower (static_cast<unsigned char> (c)));
        }
        if (match)
          break;
        if (!sub.empty ())
        {
          for (const auto &token : goal_tokens)
            if (iequals (sub, token))
            {
              match = true;
              break;
            }
        }
        if (match)
          break;
      }
      if (match)
        result.push_back (adv);
    }

    // Sort by priority desc, then id asc
    std::sort (result.begin (), result.end (),
               [] (const RegisteredAdviser &a, const RegisteredAdviser &b)
               {
                 if (a.priority != b.priority)
                   return a.priority > b.priority;
                 return a.id.value () < b.id.value ();
               });
    return result;
  }

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
      {
        impl_->advisers[row.id] = adviser;
        // No separate map needed; supports_continuation is now on the
        // RegisteredAdviser struct itself.
      }
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
    // method, but in practice the two are kept in sync
    // (finalize_worker_promotion writes both).
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
    if (dot == std::string::npos || dot != method.rfind ('.') || dot == 0
        || dot + 1 == method.size ())
      return false;
    auto valid_segment
      = [] (const std::string &s, size_t start, size_t len) -> bool
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
  //
  // Promotion is now all-or-nothing: EVERY capability method must pass
  // ADR-031 format validation before we write manifest.json, insert the
  // `agents` row, insert any capability rows, or touch the in-memory
  // Registry. If any capability fails validation, nothing is written and
  // this returns false — the caller must treat that exactly like a Code
  // Writer generation failure (i.e. feed it into Forge's normal retry
  // path), not attempt a partial registration. Previously the `agents`
  // row was inserted first and invalid capabilities were silently
  // skipped one-by-one, which could leave a registered worker with zero
  // usable capabilities (observed in production via the misrouted
  // glossary-adviser incident).
  // -----------------------------------------------------------------------
  bool Registry::finalize_worker_promotion (const ForgePipelineJob &job,
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
      return false;
    }

    // ADR-031: worker.py and worker_impl.py are written by
    // ForgeCoordinator::promote_worker before this call.
    // This function only writes manifest.json and handles DB/in-memory sync.
    auto code_path = worker_dir / "worker.py";

    // ---- Pass 1: validate ALL capabilities before writing anything. ----
    rapidjson::Document cap_doc;
    cap_doc.Parse (capability_json.c_str ());
    if (cap_doc.HasParseError () || !cap_doc.HasMember ("capabilities")
        || !cap_doc["capabilities"].IsArray ())
    {
      spdlog::error (
        "[registry] finalize_worker_promotion: worker '{}' manifest has no "
        "valid 'capabilities' array — rejecting promotion (treat as Code "
        "Writer generation failure, eligible for Forge retry)",
        job.id);
      return false;
    }

    for (const auto &cap : cap_doc["capabilities"].GetArray ())
    {
      if (!cap.IsObject () || !cap.HasMember ("method")
          || !cap["method"].IsString ())
      {
        spdlog::error (
          "[registry] finalize_worker_promotion: worker '{}' has a "
          "capability entry missing a valid 'method' field — rejecting "
          "entire promotion", job.id);
        return false;
      }

      const std::string method = cap["method"].GetString ();
      if (!is_valid_method (method))
      {
        spdlog::error (
          "[registry] finalize_worker_promotion: invalid method format "
          "'{}' for worker '{}' — rejecting entire promotion (ADR-031); "
          "no partial registration allowed", method, job.id);
        return false;
      }
    }

    // ---- Pass 2: everything validated — commit manifest, DB, memory. ----

    auto manifest_path = worker_dir / "manifest.json";
    {
      std::ofstream out (manifest_path);
      if (!out)
      {
        spdlog::error ("[registry] cannot write manifest to {}",
                       manifest_path.string ());
        return false;
      }
      out << capability_json;
    }

    // Insert agent record — only reached once every capability is known
    // to be valid, so this can no longer produce an empty-shell worker.
    db.insert_agent (job.id, "worker", code_path.string (), capability_json);

    RegisteredExecutor executor;
    executor.id = ClientId (job.id);
    executor.name = job.id;
    executor.binary_path = code_path.string ();

    for (const auto &cap : cap_doc["capabilities"].GetArray ())
    {
      const std::string method = cap["method"].GetString ();
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

      CommandSchema cmd;
      cmd.name = method;
      cmd.description = desc;
      executor.commands.push_back (std::move (cmd));
    }

    // Update forge job status to promoted
    db.update_forge_pipeline_job_status (job.id, ForgeStatus::promoted);

    // Sync in-memory registry
    for (const auto &cmd : executor.commands)
    {
      impl_->command_to_worker[cmd.name] = job.id;
      impl_->command_schemas[cmd.name] = cmd;
    }
    impl_->workers[job.id] = std::move (executor);

    spdlog::info ("[registry] worker '{}' promoted and registered", job.id);
    return true;
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
