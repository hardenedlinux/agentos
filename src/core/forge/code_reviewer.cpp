#include "agentos/forge/code_reviewer.h"
#include "agentos/capability.h"
#include "agentos/error_utils.h"
#include "agentos/home_init.h"
#include "agentos/llm_client.h"
#include "agentos/llm_proxy.h"
#include "agentos/types.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace agentos::forge
{

  namespace
  {

    // ---------------------------------------------------------------------------
    // Helper: read entire file into string
    // ---------------------------------------------------------------------------
    std::string read_file (const std::string &path)
    {
      std::ifstream ifs (path);
      if (!ifs.is_open ())
        return {};
      std::ostringstream oss;
      oss << ifs.rdbuf ();
      return oss.str ();
    }

    // ---------------------------------------------------------------
    // Helper: write string to file
    // ---------------------------------------------------------------
    bool write_file (const std::string &path, const std::string &content)
    {
      std::ofstream ofs (path);
      if (!ofs.is_open ())
        return false;
      ofs << content;
      return ofs.good ();
    }

    // ---------------------------------------------------------------
    // Helper: generate mock input JSON from input_schema
    // Schema values are plain type-name strings: "integer", "string", "path"
    // ---------------------------------------------------------------
    std::string generate_mock_input (const rapidjson::Value &schema,
                                     const std::string &scratch_dir)
    {
      rapidjson::Document mock;
      mock.SetObject ();
      auto &alloc = mock.GetAllocator ();

      for (auto it = schema.MemberBegin (); it != schema.MemberEnd (); ++it)
        {
          const std::string field_name = it->name.GetString ();
          if (!it->value.IsString ())
            continue;
          const std::string type = it->value.GetString ();

          if (type == "integer" || type == "int")
            {
          mock.AddMember (rapidjson::Value (field_name.c_str (), alloc).Move (),
                          rapidjson::Value (3).Move (), // avoid to use 0
                          alloc);
        }
        else if (type == "path")
        {
          std::string tmp_path = scratch_dir + "/" + field_name + "_mock";
          mock.AddMember (rapidjson::Value (field_name.c_str (), alloc).Move (),
                          rapidjson::Value (tmp_path.c_str (), alloc).Move (),
                          alloc);
        }
        else
        {
          // "string" or any unrecognised type → empty string
          mock.AddMember (rapidjson::Value (field_name.c_str (), alloc).Move (),
                          rapidjson::Value ("").Move (), alloc);
        }
      }

      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> writer (buf);
      mock.Accept (writer);
      return buf.GetString ();
    }

    // ---------------------------------------------------------------------------
    // Helper: validate output JSON against output_schema
    // Schema values are plain type-name strings; returns empty string on
    // success.
    // ---------------------------------------------------------------------------
    std::string validate_output_schema (const rapidjson::Value &output,
                                        const rapidjson::Value &schema)
    {
      for (auto it = schema.MemberBegin (); it != schema.MemberEnd (); ++it)
      {
        const std::string field_name = it->name.GetString ();
        if (!it->value.IsString ())
          continue;
        const std::string expected_type = it->value.GetString ();

        auto out_it = output.FindMember (field_name.c_str ());
        if (out_it == output.MemberEnd ())
          return "missing field '" + field_name + "' in output";

        const rapidjson::Value &val = out_it->value;
        if (expected_type == "integer" || expected_type == "int")
        {
          if (!val.IsInt ())
            return "field '" + field_name + "' expected integer";
        }
        else if (expected_type == "string" || expected_type == "path")
        {
          if (!val.IsString ())
            return "field '" + field_name + "' expected string";
        }
      }
      return {};
    }

    // ---------------------------------------------------------------------------
    // Helper: fork/exec worker inside a basic sandbox, return exit code.
    // Negative return values are internal errors:
    //   -1  fork failed
    //   -2  waitpid error
    //   -3  timeout
    //   -4  child killed by signal
    // ---------------------------------------------------------------------------
    int run_sandbox (const std::string &worker_binary,
                     const std::string &code_path,
                     const std::string &mock_input_path,
                     const std::string &scratch_dir, int timeout_seconds,
                     std::string &output_content)
    {
      const std::string output_path = scratch_dir + "/sandbox_output.json";

      pid_t pid = fork ();
      if (pid == -1)
        return -1;

      if (pid == 0)
      {
        /* Best-effort namespace isolation; degrade gracefully in test
           environments where CAP_SYS_ADMIN is unavailable. Production
           deployments will have the necessary privileges via the systemd
           user service.
        */
        if (namespace_isolation_available ())
          unshare (CLONE_NEWNS | CLONE_NEWNET);

        // Redirect stdin ← mock input
        int in_fd = open (mock_input_path.c_str (), O_RDONLY);
        if (in_fd == -1)
          _exit (127);
        dup2 (in_fd, STDIN_FILENO);
        close (in_fd);

        // Redirect stdout/stderr → output file
        int out_fd
          = open (output_path.c_str (), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd == -1)
          _exit (127);
        dup2 (out_fd, STDOUT_FILENO);
        dup2 (out_fd, STDERR_FILENO);
        close (out_fd);

        // exec: interpreter <script>
        execlp (worker_binary.c_str (), worker_binary.c_str (),
                code_path.c_str (), nullptr);
        _exit (127);
      }

      // Parent: poll with timeout
      const auto deadline = std::chrono::steady_clock::now ()
                            + std::chrono::seconds (timeout_seconds);

      while (true)
      {
        int status;
        pid_t ret = waitpid (pid, &status, WNOHANG);

        if (ret == pid)
        {
          if (WIFEXITED (status))
          {
            output_content = read_file (output_path);
            return WEXITSTATUS (status);
          }
          // killed by signal
          return -4;
        }

        if (ret == -1)
        {
          kill (pid, SIGKILL);
          waitpid (pid, nullptr, 0);
          return -2;
        }

        if (std::chrono::steady_clock::now () >= deadline)
        {
          kill (pid, SIGKILL);
          waitpid (pid, nullptr, 0);
          return -3;
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (50));
      }
    }

  } // anonymous namespace

  // ---------------------------------------------------------------------------
  // code_reviewer — main entry point
  // ---------------------------------------------------------------------------
  std::string code_reviewer (const std::string &input_json)
  {
    // ── 1. Parse input
    // ────────────────────────────────────────────────────────
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

    auto wo_it = doc.FindMember ("writer_output");
    if (wo_it == doc.MemberEnd () || !wo_it->value.IsObject ())
      return make_error ("missing or invalid 'writer_output'");
    const rapidjson::Value &writer_output = wo_it->value;

    std::string code, language, understanding;
    if (!require_string (writer_output, "code", code))
      return make_error ("missing or invalid 'writer_output.code'");
    if (!require_string (writer_output, "language", language))
      return make_error ("missing or invalid 'writer_output.language'");
    if (!require_string (writer_output, "understanding", understanding))
      return make_error ("missing or invalid 'writer_output.understanding'");

    auto cap_it = writer_output.FindMember ("capability");
    if (cap_it == writer_output.MemberEnd () || !cap_it->value.IsObject ())
      return make_error ("missing or invalid 'writer_output.capability'");
    const rapidjson::Value &capability = cap_it->value;

    // ── 2. Enforce Layer pre-check (ADR-009 Layer B) ─────────────────────────
    // Policy violations are terminal — no retry, no LLM call.
    {
      auto net_it = capability.FindMember ("network");
      if (net_it != capability.MemberEnd () && net_it->value.IsBool ()
          && net_it->value.GetBool ())
        return make_reject_verdict (
          task_id, "policy violation: network access declared");

      auto exec_it = capability.FindMember ("exec");
      if (exec_it != capability.MemberEnd () && exec_it->value.IsBool ()
          && exec_it->value.GetBool ())
        return make_reject_verdict (task_id,
                                    "policy violation: exec access declared");
    }

    // ── 3. Scratch directory
    // ──────────────────────────────────────────────────
    const std::filesystem::path scratch_dir
      = agentos_home () / "forge" / forge_job_id;
    {
      std::error_code ec;
      std::filesystem::create_directories (scratch_dir, ec);
      if (ec)
        return make_error ("failed to create scratch directory: "
                           + ec.message ());
    }
    const std::string scratch = scratch_dir.string ();

    // ── 4. Write code file
    // ────────────────────────────────────────────────────
    std::string worker_binary;
    std::string code_ext;
    if (language == "python")
    {
      worker_binary = "/usr/bin/python3";
      code_ext = ".py";
    }
    else if (language == "guile")
    {
      worker_binary = "/usr/bin/guile";
      code_ext = ".scm";
    }
    else
      return make_error ("unsupported language: " + language);

    const std::string code_path = scratch + "/sandbox_probe" + code_ext;
    if (!write_file (code_path, code))
      return make_error ("failed to write code file");

    // ── 5. Generate mock input
    // ────────────────────────────────────────────────
    rapidjson::Value empty_schema (rapidjson::kObjectType);
    const rapidjson::Value &input_schema = [&] () -> const rapidjson::Value &
    {
      auto it = requirement.FindMember ("input_schema");
      if (it != requirement.MemberEnd () && it->value.IsObject ())
        return it->value;
      return empty_schema;
    }();

    const std::string mock_input = generate_mock_input (input_schema, scratch);
    const std::string mock_input_path = scratch + "/mock_input.json";
    if (!write_file (mock_input_path, mock_input))
      return make_error ("failed to write mock input file");

    // ── 6. Sandbox execution
    // ──────────────────────────────────────────────────
    std::string sandbox_output;

    if (!namespace_isolation_available ())
      spdlog::warn ("[code_reviewer] namespace isolation unavailable; "
                    "sandbox running without CLONE_NEWNS/CLONE_NEWNET");

    const int exit_code
      = run_sandbox (worker_binary, code_path, mock_input_path, scratch,
                     /*timeout_s=*/30, sandbox_output);

    if (exit_code == -1)
      return make_reject_verdict (task_id, "sandbox fork failed");
    if (exit_code == -2)
      return make_reject_verdict (task_id, "sandbox waitpid error");
    if (exit_code == -3)
      return make_reject_verdict (task_id, "sandbox timeout");
    if (exit_code == -4)
      return make_reject_verdict (task_id, "sandbox child killed by signal");
    if (exit_code != 0)
      return make_reject_verdict (task_id, "worker exited with code "
                                             + std::to_string (exit_code));

    // ── 7. Output schema validation
    // ───────────────────────────────────────────
    const rapidjson::Value &output_schema = [&] () -> const rapidjson::Value &
    {
      auto it = requirement.FindMember ("output_schema");
      if (it != requirement.MemberEnd () && it->value.IsObject ())
        return it->value;
      return empty_schema;
    }();

    rapidjson::Document output_doc;
    output_doc.Parse (sandbox_output.c_str ());
    if (output_doc.HasParseError () || !output_doc.IsObject ())
      return make_reject_verdict (task_id, "sandbox output is not valid JSON");

    const std::string schema_err
      = validate_output_schema (output_doc, output_schema);
    if (!schema_err.empty ())
      return make_reject_verdict (task_id, "output schema validation failed: "
                                             + schema_err);

    // ── 8. LLM review
    // ───────────────────────────────────────────────────────── Read skill.md
    const char *skill_path_env = std::getenv ("AGENTOS_ADVISER_SKILL_PATH");
    std::string system_prompt;
    if (skill_path_env)
      system_prompt = read_file (skill_path_env);
    if (system_prompt.empty ())
      system_prompt
        = "You are a code reviewer. Evaluate the code for functional "
          "correctness against the requirement. "
          "Respond with JSON only: "
          "{\"status\": \"accept\" or \"reject\", \"reason\": \"...\"}";

    // Read LLM config (ADR-018 AGENTOS_ADVISER_* prefix)
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
    const int max_tokens = max_tokens_env ? std::atoi (max_tokens_env) : 1024;
    const int llm_timeout = timeout_env ? std::atoi (timeout_env) : 60;

    // Build user prompt
    std::string description;
    {
      auto it = requirement.FindMember ("description");
      if (it != requirement.MemberEnd () && it->value.IsString ())
        description = it->value.GetString ();
    }
    const std::string user_prompt
      = "Requirement:\n" + description + "\n\nWriter's understanding:\n"
        + understanding + "\n\nCode:\n```\n" + code + "\n```"
        + "\n\nSandbox result: exit code 0, output: " + sandbox_output
        + "\n\nRespond with JSON only: "
          "{\"status\": \"accept\" or \"reject\", \"reason\": \"...\"}";

    // Determine api_path from base_url
    const bool is_anthropic
      = base_url.find ("anthropic.com") != std::string::npos;
    const std::string api_path
      = is_anthropic ? "/v1/messages" : "/v1/chat/completions";

    LlmRequest llm_req;
    llm_req.base_url = base_url;
    llm_req.api_key = api_key;
    llm_req.model = model;
    llm_req.system_prompt = system_prompt;
    llm_req.user_prompt = user_prompt;
    llm_req.max_tokens = max_tokens;
    llm_req.api_path = api_path;

    // Construct proxy locally (pool_size=1, uses resolved timeout)
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

    // Parse LLM response
    rapidjson::Document llm_doc;
    llm_doc.Parse (llm_result.value.content.c_str ());
    if (llm_doc.HasParseError () || !llm_doc.IsObject ())
      return make_error ("LLM response is not valid JSON: "
                         + llm_result.value.content);

    auto status_it = llm_doc.FindMember ("status");
    if (status_it == llm_doc.MemberEnd () || !status_it->value.IsString ())
      return make_error ("LLM response missing 'status' field");

    const std::string status = status_it->value.GetString ();
    if (status != "accept" && status != "reject")
      return make_error ("LLM response status must be 'accept' or 'reject'");

    std::string reason;
    auto reason_it = llm_doc.FindMember ("reason");
    if (reason_it != llm_doc.MemberEnd () && reason_it->value.IsString ())
      reason = reason_it->value.GetString ();

    // ── 9. Build final verdict
    // ────────────────────────────────────────────────
    rapidjson::Document verdict;
    verdict.SetObject ();
    auto &alloc = verdict.GetAllocator ();
    verdict.AddMember (
      "task_id", rapidjson::Value (task_id.c_str (), alloc).Move (), alloc);
    verdict.AddMember (
      "status", rapidjson::Value (status.c_str (), alloc).Move (), alloc);
    verdict.AddMember (
      "reason", rapidjson::Value (reason.c_str (), alloc).Move (), alloc);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer (buf);
    verdict.Accept (writer);
    return buf.GetString ();
  }

} // namespace agentos::forge
