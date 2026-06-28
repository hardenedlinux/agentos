#include "agentos/code_writer.h"
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
  std::string code_writer (const std::string &input_json, LlmProxy &proxy)
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
    if (!api_key_env || std::strlen (api_key_env) == 0)
      return make_error ("LLM API key not set (AGENTOS_ADVISER_API_KEY)");

    const std::string base_url
      = base_url_env ? base_url_env : "https://api.anthropic.com";
    const std::string api_key = api_key_env;
    const std::string model = model_env ? model_env : "claude-opus-4-5";
    const int max_tokens = max_tokens_env ? std::atoi (max_tokens_env) : 2048;

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
      += "\n\nYou MUST respond with ONLY a JSON object. No markdown fences, "
         "no explanation, no prose before or after. The JSON object MUST "
         "contain ALL of these fields with EXACTLY these names:\n"
         "{\n"
         "  \"understanding\": \"<your interpretation of the requirement>\",\n"
         "  \"language\": \"python\",\n"
         "  \"entry_point\": \"run\",\n"
         "  \"impl_code\": \"<complete worker_impl.py source — only business "
         "logic, no main(), no stdin, no file I/O>\",\n"
         "  \"signatures\": {\n"
         "    \"run\": {\n"
         "      \"signature\": \"(task: dict) -> dict\",\n"
         "      \"doc\": \"<one-line description of what run() does>\"\n"
         "    }\n"
         "  },\n"
         "  \"capability\": {\n"
         "    \"network\": false,\n"
         "    \"fs_read\": [],\n"
         "    \"fs_write\": [],\n"
         "    \"exec\": false\n"
         "  },\n"
         "  \"notes\": \"\"\n"
         "}\n"
         "impl_code must define a top-level function run(task: dict) -> dict. "
         "Do NOT include main(), sys.stdin, open(), or os.environ in impl_code. "
         "The signatures object must have an entry for every public function. "
         "Missing any field will cause the entire attempt to be rejected. "
         "network and exec must be false unless the requirement explicitly "
         "demands them.";

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
    // Strip markdown fences if present (LLMs often wrap JSON in ```json ... ```)
    std::string raw_content = llm_result.value.content;
    {
      // Remove leading ```json or ``` fence
      auto fence_start = raw_content.find ("```");
      if (fence_start != std::string::npos)
      {
        auto content_start = raw_content.find ('\n', fence_start);
        if (content_start != std::string::npos)
        {
          auto fence_end = raw_content.rfind ("```");
          if (fence_end != std::string::npos && fence_end > content_start)
            raw_content = raw_content.substr (content_start + 1,
                                              fence_end - content_start - 1);
        }
      }
      // Trim leading/trailing whitespace
      const auto trim_start = raw_content.find_first_not_of (" \t\r\n");
      const auto trim_end = raw_content.find_last_not_of (" \t\r\n");
      if (trim_start != std::string::npos)
        raw_content = raw_content.substr (trim_start,
                                          trim_end - trim_start + 1);
    }

    rapidjson::Document llm_doc;
    llm_doc.Parse (raw_content.c_str ());
    if (llm_doc.HasParseError () || !llm_doc.IsObject ())
      return make_error ("LLM response is not valid JSON: " + raw_content);

    auto require_llm_string = [&] (const char *key, std::string &out) -> bool
    {
      auto it = llm_doc.FindMember (key);
      if (it == llm_doc.MemberEnd () || !it->value.IsString ())
        return false;
      out = it->value.GetString ();
      return true;
    };

    std::string understanding, language, entry_point, impl_code;
    if (!require_llm_string ("understanding", understanding))
    {
      spdlog::error ("[code_writer] LLM response missing 'understanding'. "
                     "Raw response: {}",
                     raw_content.substr (0, 500));
      return make_error ("LLM response missing 'understanding'");
    }
    if (!require_llm_string ("language", language))
    {
      spdlog::error ("[code_writer] LLM response missing 'language'. "
                     "Raw response: {}",
                     raw_content.substr (0, 500));
      return make_error ("LLM response missing 'language'");
    }
    if (!require_llm_string ("entry_point", entry_point))
    {
      spdlog::error ("[code_writer] LLM response missing 'entry_point'. "
                     "Raw response: {}",
                     raw_content.substr (0, 500));
      return make_error ("LLM response missing 'entry_point'");
    }
    // ADR-031: two-file structure — impl_code replaces code
    if (!require_llm_string ("impl_code", impl_code))
    {
      spdlog::error ("[code_writer] LLM response missing 'impl_code'. "
                     "Raw response: {}",
                     raw_content.substr (0, 500));
      return make_error ("LLM response missing 'impl_code'");
    }

    // signatures is an object — validate it exists and is an object
    auto sigs_it = llm_doc.FindMember ("signatures");
    if (sigs_it == llm_doc.MemberEnd () || !sigs_it->value.IsObject ())
    {
      spdlog::error ("[code_writer] LLM response missing 'signatures' object. "
                     "Raw response: {}",
                     raw_content.substr (0, 500));
      return make_error ("LLM response missing 'signatures' object");
    }

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

    // ── 7. Build output JSON (ADR-031 two-file structure)
    // task_id injected by forge_coordinator after validation.
    // {
    //   "understanding": "...",
    //   "language":      "python",
    //   "entry_point":   "run",
    //   "impl_code":     "<worker_impl.py source>",
    //   "signatures":    { "<fn>": { "signature": "...", "doc": "..." }, ... },
    //   "capability":    { ... },
    //   "notes":         "..."
    // }
    rapidjson::Document out;
    out.SetObject ();
    auto &alloc = out.GetAllocator ();

    out.AddMember ("understanding",
                   rapidjson::Value (understanding.c_str (), alloc).Move (),
                   alloc);
    out.AddMember ("language",
                   rapidjson::Value (language.c_str (), alloc).Move (), alloc);
    out.AddMember ("entry_point",
                   rapidjson::Value (entry_point.c_str (), alloc).Move (),
                   alloc);
    out.AddMember ("impl_code",
                   rapidjson::Value (impl_code.c_str (), alloc).Move (), alloc);

    // Deep-copy signatures object from LLM response
    rapidjson::Value sigs_copy (sigs_it->value, alloc);
    out.AddMember ("signatures", sigs_copy.Move (), alloc);

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
