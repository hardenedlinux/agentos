#include "agentos/llm_client.h"
#include "agentos/llm_proxy.h"
#include "agentos/types.h"
#include <cstdlib>
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

    // Read the skill prompt from the file specified by
    // AGENTOS_ADVISER_SKILL_PATH, or fall back to
    // $AGENTOS_HOME/advisers/code-writer/skill.md.
    std::string read_skill_prompt ()
    {
      const char *skill_path_env = std::getenv ("AGENTOS_ADVISER_SKILL_PATH");
      std::filesystem::path skill_path;

      if (skill_path_env && *skill_path_env)
      {
        skill_path = skill_path_env;
      }
      else
      {
        const char *home_env = std::getenv ("AGENTOS_HOME");
        if (home_env && *home_env)
        {
          skill_path = std::filesystem::path (home_env) / "advisers"
                       / "code-writer" / "skill.md";
        }
        else
        {
          const char *home_dir = std::getenv ("HOME");
          if (home_dir && *home_dir)
          {
            skill_path = std::filesystem::path (home_dir) / ".agentos"
                         / "advisers" / "code-writer" / "skill.md";
          }
          else
          {
            // No HOME either – use a hard‑coded fallback (shouldn't happen in
            // practice)
            skill_path = "/tmp/agentos/advisers/code-writer/skill.md";
          }
        }
      }

      std::ifstream ifs (skill_path);
      if (!ifs.is_open ())
      {
        return {}; // empty string signals error
      }
      std::stringstream ss;
      ss << ifs.rdbuf ();
      return ss.str ();
    }

  } // anonymous namespace

  std::string code_writer (const std::string &input_json)
  {
    // Parse input JSON
    rapidjson::Document input;
    input.Parse (input_json.c_str ());
    if (input.HasParseError ())
    {
      rapidjson::Document err;
      err.SetObject ();
      err.AddMember ("status", "error", err.GetAllocator ());
      err.AddMember ("reason", "Failed to parse input JSON",
                     err.GetAllocator ());
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      err.Accept (w);
      return buf.GetString ();
    }

    // Extract fields
    std::string task_id
      = input.HasMember ("task_id") && input["task_id"].IsString ()
          ? input["task_id"].GetString ()
          : "unknown";
    std::string requirement
      = input.HasMember ("requirement") && input["requirement"].IsString ()
          ? input["requirement"].GetString ()
          : "";
    std::string language
      = input.HasMember ("language") && input["language"].IsString ()
          ? input["language"].GetString ()
          : "python";

    if (requirement.empty ())
    {
      rapidjson::Document err;
      err.SetObject ();
      err.AddMember ("status", "error", err.GetAllocator ());
      err.AddMember ("reason", "Missing 'requirement' field",
                     err.GetAllocator ());
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      err.Accept (w);
      return buf.GetString ();
    }

    // Read the skill prompt (ADR-018)
    std::string skill_prompt = read_skill_prompt ();
    if (skill_prompt.empty ())
    {
      rapidjson::Document err;
      err.SetObject ();
      err.AddMember ("status", "error", err.GetAllocator ());
      err.AddMember ("reason",
                     "Could not read skill prompt file "
                     "(AGENTOS_ADVISER_SKILL_PATH or default skill.md)",
                     err.GetAllocator ());
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      err.Accept (w);
      return buf.GetString ();
    }

    // Read LLM configuration from environment
    const char *base_url_env = std::getenv ("AGENTOS_LLM_BASE_URL");
    const char *api_key_env = std::getenv ("AGENTOS_LLM_API_KEY");
    const char *model_env = std::getenv ("AGENTOS_LLM_MODEL");

    std::string base_url
      = base_url_env ? base_url_env : "https://api.anthropic.com";
    std::string api_key = api_key_env ? api_key_env : "";
    std::string model = model_env ? model_env : "claude-opus-4-5";

    if (api_key.empty ())
    {
      rapidjson::Document err;
      err.SetObject ();
      err.AddMember ("status", "error", err.GetAllocator ());
      err.AddMember ("reason",
                     "AGENTOS_LLM_API_KEY environment variable not set",
                     err.GetAllocator ());
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      err.Accept (w);
      return buf.GetString ();
    }

    // Determine API path based on provider
    bool is_anthropic = base_url.find ("anthropic.com") != std::string::npos;
    std::string api_path
      = is_anthropic ? "/v1/messages" : "/v1/chat/completions";

    // Build LLM request
    LlmRequest llm_req;
    llm_req.base_url = base_url;
    llm_req.api_key = api_key;
    llm_req.model = model;
    llm_req.system_prompt = skill_prompt;
    llm_req.user_prompt = requirement;
    llm_req.max_tokens = 2048;
    llm_req.api_path = api_path;

    // Create a static proxy (initialised once)
    static LlmProxy proxy (1, 30);

    // Enqueue request and wait for result
    auto fut = proxy.enqueue (llm_req);
    auto result = fut.get ();

    if (!result.ok)
      {
        rapidjson::Document err;
        err.SetObject ();
        err.AddMember ("status", "error", err.GetAllocator ());
        err.AddMember ("reason", result.errors.message, err.GetAllocator ());
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w (buf);
        err.Accept (w);
        return buf.GetString ();
      }

    std::string code = result.value.content;

    // Build output JSON (same structure as before)
    rapidjson::Document out;
    out.SetObject ();
    out.AddMember (
      "task_id",
      rapidjson::Value (task_id.c_str (), out.GetAllocator ()).Move (),
      out.GetAllocator ());
    out.AddMember ("understanding", "Code generated by LLM.",
                   out.GetAllocator ());
    out.AddMember (
      "language",
      rapidjson::Value (language.c_str (), out.GetAllocator ()).Move (),
      out.GetAllocator ());
    out.AddMember ("entry_point", "main", out.GetAllocator ());
    out.AddMember (
      "code", rapidjson::Value (code.c_str (), out.GetAllocator ()).Move (),
      out.GetAllocator ());

    rapidjson::Value cap (rapidjson::kObjectType);
    cap.AddMember ("network", false, out.GetAllocator ());
    cap.AddMember ("fs_read", rapidjson::kArrayType, out.GetAllocator ());
    cap.AddMember ("fs_write", rapidjson::kArrayType, out.GetAllocator ());
    cap.AddMember ("exec", false, out.GetAllocator ());
    out.AddMember ("capability", cap, out.GetAllocator ());
    out.AddMember ("notes", "", out.GetAllocator ());

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    out.Accept (w);
    return buf.GetString ();
  }

} // namespace agentos::forge
