#include "agentos/forge/code_writer.h"
#include "agentos/error_utils.h"
#include "agentos/llm_proxy.h"
#include "agentos/types.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <string>

namespace agentos::forge
{

  namespace
  {

    // ---------------------------------------------------------------------------
    // Helper: read entire file into string
    // ---------------------------------------------------------------------------
    std::string read_file (const std::filesystem::path &path)
    {
      std::ifstream ifs (path);
      if (!ifs.is_open ())
        return {};
      std::ostringstream oss;
      oss << ifs.rdbuf ();
      return oss.str ();
    }

    // ---------------------------------------------------------------------------
    // Helper: resolve skill.md path (ADR-018)
    // Priority: AGENTOS_ADVISER_SKILL_PATH → AGENTOS_HOME → HOME/.agentos
    // ---------------------------------------------------------------------------
    std::string read_skill_prompt ()
    {
      const char *skill_env = std::getenv ("AGENTOS_ADVISER_SKILL_PATH");
      if (skill_env && *skill_env)
        return read_file (skill_env);

      std::filesystem::path base;
      const char *home_env = std::getenv ("AGENTOS_HOME");
      if (home_env && *home_env)
        base = home_env;
      else if (const char *home_dir = std::getenv ("HOME");
               home_dir && *home_dir)
        base = std::filesystem::path (home_dir) / ".agentos";
      else
        return {};

      return read_file (base / "advisers" / "code-writer" / "skill.md");
    }

    // ---------------------------------------------------------------------------
    // Helper: serialise a rapidjson Value to string
    // ---------------------------------------------------------------------------
    std::string serialise (const rapidjson::Value &v)
    {
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      v.Accept (w);
      return buf.GetString ();
    }

  } // anonymous namespace

  // ---------------------------------------------------------------------------
  // code_writer — main entry point
  // ---------------------------------------------------------------------------
  std::string code_writer (const std::string &input_json)
  {
    // ── 1. Parse input
    // ──────────────────────────────────────────────────────── Expected
    // structure per ADR-019:
    // {
    //   "task_id":      "...",
    //   "forge_job_id": "...",
    //   "requirement": {
    //     "description":   "...",
    //     "input_schema":  { ... },
    //     "output_schema": { ... }
    //   },
    //   "feedback": ""    // empty on first attempt; Reviewer reason on retry
    // }
    rapidjson::Document doc;
    doc.Parse (input_json.c_str ());
    if (doc.HasParseError () || !doc.IsObject ())
      return make_error ("invalid input JSON");

    auto require_string = [&] (const rapidjson::Value &obj, const char *key,
                               std::string &out) -> bool
    {
      auto it = obj.FindMember (key);
      if (it == obj.MemberEnd () || !it->value.IsString ())
        return false;
      out = it->value.GetString ();
      return true;
    };

    std::string task_id, forge_job_id;
    if (!require_string (doc, "task_id", task_id))
      return make_error ("missing or invalid 'task_id'");
    if (!require_string (doc, "forge_job_id", forge_job_id))
      return make_error ("missing or invalid 'forge_job_id'");

    auto req_it = doc.FindMember ("requirement");
    if (req_it == doc.MemberEnd () || !req_it->value.IsObject ())
      return make_error ("missing or invalid 'requirement'");
    const rapidjson::Value &requirement = req_it->value;

    std::string description;
    if (!require_string (requirement, "description", description))
      return make_error ("missing or invalid 'requirement.description'");

    // feedback is optional — empty string on first attempt
    std::string feedback;
    {
      auto it = doc.FindMember ("feedback");
      if (it != doc.MemberEnd () && it->value.IsString ())
        feedback = it->value.GetString ();
    }

    // ── 2. Read skill prompt (ADR-018)
    // ────────────────────────────────────────
    std::string system_prompt = read_skill_prompt ();
    if (system_prompt.empty ())
      return make_error ("could not read skill.md "
                         "(AGENTOS_ADVISER_SKILL_PATH or default path)");

    // ── 3. Read LLM config (ADR-018 AGENTOS_ADVISER_* prefix) ────────────────
    const char *base_url_env = std::getenv ("AGENTOS_ADVISER_BASE_URL");
    const char *api_key_env = std::getenv ("AGENTOS_ADVISER_API_KEY");
    const char *model_env = std::getenv ("AGENTOS_ADVISER_MODEL");
    const char *max_tokens_env = std::getenv ("AGENTOS_ADVISER_MAX_TOKENS");
    const char *timeout_env = std::getenv ("AGENTOS_ADVISER_TIMEOUT_S");

    if (!api_key_env || std::strlen (api_key_env) == 0)
      return make_error ("LLM API key not set (AGENTOS_ADVISER_API_KEY)");

    const std::string base_url
      = base_url_env ? base_url_env : "https://api.anthropic.com";
    const std::string api_key = api_key_env;
    const std::string model = model_env ? model_env : "claude-opus-4-5";
    const int max_tokens = max_tokens_env ? std::atoi (max_tokens_env) : 2048;
    const int llm_timeout = timeout_env ? std::atoi (timeout_env) : 120;

    const bool is_anthropic
      = base_url.find ("anthropic.com") != std::string::npos;
    const std::string api_path
      = is_anthropic ? "/v1/messages" : "/v1/chat/completions";

    // ── 4. Build user prompt
    // ────────────────────────────────────────────────── Include requirement
    // schemas so the LLM knows what the worker must consume and produce.
    // Include feedback when retrying (ADR-019).
    std::string input_schema_str = "{}";
    std::string output_schema_str = "{}";
    {
      auto it = requirement.FindMember ("input_schema");
      if (it != requirement.MemberEnd () && it->value.IsObject ())
        input_schema_str = serialise (it->value);
    }
    {
      auto it = requirement.FindMember ("output_schema");
      if (it != requirement.MemberEnd () && it->value.IsObject ())
        output_schema_str = serialise (it->value);
    }

    std::string user_prompt
      = "Requirement:\n" + description + "\n\nInput schema (JSON):\n"
        + input_schema_str + "\n\nOutput schema (JSON):\n" + output_schema_str;

    if (!feedback.empty ())
      user_prompt += "\n\nPrevious attempt was rejected for the following "
                     "reason — treat this as a directed correction:\n"
                     + feedback;

    user_prompt
      += "\n\nRespond with a JSON object only (no markdown, no explanation):\n"
         "{\n"
         "  \"understanding\": \"<your interpretation of the requirement>\",\n"
         "  \"language\": \"python\" | \"guile\",\n"
         "  \"entry_point\": \"<function or module entry point>\",\n"
         "  \"code\": \"<full source code as a single string>\",\n"
         "  \"capability\": {\n"
         "    \"network\": false,\n"
         "    \"fs_read\": [],\n"
         "    \"fs_write\": [],\n"
         "    \"exec\": false\n"
         "  },\n"
         "  \"notes\": \"<optional remarks for the reviewer>\"\n"
         "}\n"
         "The capability block must accurately reflect what the code actually "
         "does.\n"
         "bash is forbidden. Only python or guile are allowed.\n"
         "network and exec must be false unless the requirement explicitly "
         "demands "
         "them (they will be rejected if true).";

    // ── 5. Call LLM
    // ───────────────────────────────────────────────────────────
    LlmRequest llm_req;
    llm_req.base_url = base_url;
    llm_req.api_key = api_key;
    llm_req.model = model;
    llm_req.system_prompt = system_prompt;
    llm_req.user_prompt = user_prompt;
    llm_req.max_tokens = max_tokens;
    llm_req.api_path = api_path;

    LlmProxy proxy (1, llm_timeout);
    auto fut = proxy.enqueue (llm_req);

    Result<LlmResponse> llm_result;
    try
    {
      llm_result = fut.get ();
    }
    catch (const std::exception &e)
    {
      return make_error ("LLM call threw exception: "
                         + std::string (e.what ()));
    }

    if (!llm_result.ok)
      return make_error ("LLM call failed: " + llm_result.error);

    // ── 6. Parse LLM response
    // ───────────────────────────────────────────────── LLM must return a JSON
    // object with understanding, language, entry_point, code, capability,
    // notes. Parse and validate mandatory fields.
    rapidjson::Document llm_doc;
    llm_doc.Parse (llm_result.value.content.c_str ());
    if (llm_doc.HasParseError () || !llm_doc.IsObject ())
      return make_error ("LLM response is not valid JSON: "
                         + llm_result.value.content);

    auto require_llm_string = [&] (const char *key, std::string &out) -> bool
    {
      auto it = llm_doc.FindMember (key);
      if (it == llm_doc.MemberEnd () || !it->value.IsString ())
        return false;
      out = it->value.GetString ();
      return true;
    };

    std::string understanding, language, entry_point, code;
    if (!require_llm_string ("understanding", understanding))
      return make_error ("LLM response missing 'understanding'");
    if (!require_llm_string ("language", language))
      return make_error ("LLM response missing 'language'");
    if (!require_llm_string ("entry_point", entry_point))
      return make_error ("LLM response missing 'entry_point'");
    if (!require_llm_string ("code", code))
      return make_error ("LLM response missing 'code'");

    if (language != "python" && language != "guile")
      return make_error ("LLM chose forbidden language: " + language);

    auto cap_it = llm_doc.FindMember ("capability");
    if (cap_it == llm_doc.MemberEnd () || !cap_it->value.IsObject ())
      return make_error ("LLM response missing 'capability' object");

    std::string notes;
    {
      auto it = llm_doc.FindMember ("notes");
      if (it != llm_doc.MemberEnd () && it->value.IsString ())
        notes = it->value.GetString ();
    }

    // ── 7. Build output JSON per ADR-019 contract
    // ─────────────────────────────
    // {
    //   "task_id":      "...",
    //   "understanding":"...",
    //   "language":     "python" | "guile",
    //   "entry_point":  "...",
    //   "code":         "...",
    //   "capability":   { "network": bool, "fs_read": [...], "fs_write": [...],
    //   "exec": bool }, "notes":        "..."
    // }
    rapidjson::Document out;
    out.SetObject ();
    auto &alloc = out.GetAllocator ();

    out.AddMember ("task_id",
                   rapidjson::Value (task_id.c_str (), alloc).Move (), alloc);
    out.AddMember ("understanding",
                   rapidjson::Value (understanding.c_str (), alloc).Move (),
                   alloc);
    out.AddMember ("language",
                   rapidjson::Value (language.c_str (), alloc).Move (), alloc);
    out.AddMember ("entry_point",
                   rapidjson::Value (entry_point.c_str (), alloc).Move (),
                   alloc);
    out.AddMember ("code", rapidjson::Value (code.c_str (), alloc).Move (),
                   alloc);

    // Deep-copy capability block from LLM response
    rapidjson::Value cap_copy (cap_it->value, alloc);
    out.AddMember ("capability", cap_copy.Move (), alloc);

    out.AddMember ("notes", rapidjson::Value (notes.c_str (), alloc).Move (),
                   alloc);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    out.Accept (w);
    return buf.GetString ();
  }

} // namespace agentos::forge
