#include "agentos/code_reviewer.h"
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

        // Set AGENTOS_RUN_DIR so the worker knows where to write result.json
        setenv ("AGENTOS_RUN_DIR", scratch_dir.c_str (), 1);

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
  std::string code_reviewer (const std::string &input_json, LlmProxy &proxy)
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

    std::string impl_code, language, understanding;
    // ADR-031: impl_code replaces code; fall back to code for old attempts.
    if (!require_string (writer_output, "impl_code", impl_code))
    {
      if (!require_string (writer_output, "code", impl_code))
        return make_error ("missing or invalid 'writer_output.impl_code'");
    }
    if (!require_string (writer_output, "language", language))
      return make_error ("missing or invalid 'writer_output.language'");
    if (!require_string (writer_output, "understanding", understanding))
      return make_error ("missing or invalid 'writer_output.understanding'");

    // Extract signatures if present (LLM-generated, used to guide Reviewer).
    std::string signatures_json = "{}";
    {
      auto sigs_it = writer_output.FindMember ("signatures");
      if (sigs_it != writer_output.MemberEnd ()
          && sigs_it->value.IsObject ())
      {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> sw (sb);
        sigs_it->value.Accept (sw);
        signatures_json = sb.GetString ();
      }
    }

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

    // ── 4. Write worker_impl.py and worker.py to scratch directory
    // ADR-031: two-file structure; sandbox runs worker.py which imports impl.
    std::string worker_binary;
    if (language == "python")
      worker_binary = "/usr/bin/python3";
    else if (language == "guile")
      worker_binary = "/usr/bin/guile";
    else
      return make_error ("unsupported language: " + language);

    // Write worker_impl.py (generated business logic).
    const std::string impl_path = scratch + "/worker_impl.py";
    if (!write_file (impl_path, impl_code))
      return make_error ("failed to write worker_impl.py");

    // Copy worker_template.py → worker.py (fixed runtime entry point).
    const std::string worker_py_path = scratch + "/worker.py";
    {
      std::filesystem::path template_src
        = agentos_home () / "skills" / "worker_template.py";
      if (std::filesystem::exists (template_src))
      {
        std::error_code ec;
        std::filesystem::copy_file (
          template_src, worker_py_path,
          std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
          spdlog::warn ("[code_reviewer] cannot copy worker template: {}",
                        ec.message ());
      }
      else
      {
        // Fallback: write impl_code directly so sandbox can still run.
        spdlog::warn ("[code_reviewer] worker_template.py missing, "
                      "writing impl directly as worker.py (degraded)");
        if (!write_file (worker_py_path, impl_code))
          return make_error ("failed to write worker.py fallback");
      }
    }
    const std::string code_path = worker_py_path;

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
    // Worker writes result.json to AGENTOS_RUN_DIR (ADR-016 contract).
    // Read that file — sandbox_output (stdout) is a log, not the result.
    const std::string result_json_path = scratch + "/result.json";
    const std::string result_json_content = read_file (result_json_path);

    const rapidjson::Value &output_schema = [&] () -> const rapidjson::Value &
    {
      auto it = requirement.FindMember ("output_schema");
      if (it != requirement.MemberEnd () && it->value.IsObject ())
        return it->value;
      return empty_schema;
    }();

    rapidjson::Document result_envelope;
    result_envelope.Parse (result_json_content.c_str ());
    if (result_envelope.HasParseError () || !result_envelope.IsObject ())
      return make_reject_verdict (task_id, "sandbox output is not valid JSON");

    // Unwrap the {status, result} envelope written by the worker.
    auto status_field = result_envelope.FindMember ("status");
    if (status_field == result_envelope.MemberEnd ()
        || !status_field->value.IsString ())
      return make_reject_verdict (task_id,
                                  "sandbox output missing 'status' field");

    if (std::string (status_field->value.GetString ()) == "error")
    {
      std::string worker_err = "worker reported error";
      auto err_it = result_envelope.FindMember ("error");
      if (err_it != result_envelope.MemberEnd ()
          && err_it->value.IsString ())
        worker_err = err_it->value.GetString ();
      return make_reject_verdict (task_id, worker_err);
    }

    // Extract the `result` payload for schema validation.
    rapidjson::Document output_doc;
    auto result_it = result_envelope.FindMember ("result");
    if (result_it != result_envelope.MemberEnd ()
        && result_it->value.IsObject ())
    {
      output_doc.CopyFrom (result_it->value, output_doc.GetAllocator ());
    }
    else
    {
      output_doc.SetObject ();
    }

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
        = "You are a code reviewer for AgentOS worker code. "
          "Evaluate the code for functional correctness against the "
          "requirement. "
          "IMPORTANT: All workers MUST read input from stdin (sys.stdin.read()) "
          "as part of the AgentOS worker contract — do NOT reject code for "
          "reading stdin, even when the input schema is empty. "
          "Workers MUST write their result to a file at "
          "os.environ['AGENTOS_RUN_DIR']/result.json — this is mandatory. "
          "Only reject if the code is functionally incorrect, unsafe, or "
          "violates the capability declaration (network/exec). "
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

    // Build user prompt — include impl_code, signatures, and sandbox result.
    // Reviewer will either accept/reject or request a test run.
    std::string description;
    {
      auto it = requirement.FindMember ("description");
      if (it != requirement.MemberEnd () && it->value.IsString ())
        description = it->value.GetString ();
    }
    const std::string user_prompt
      = "Requirement:\n" + description
        + "\n\nWriter's understanding:\n" + understanding
        + "\n\nFunction signatures (LLM-generated):\n" + signatures_json
        + "\n\nworker_impl.py source:\n```python\n" + impl_code + "\n```"
        + "\n\nSandbox run result (worker.py entry point):\n"
          "exit code: " + std::to_string (exit_code == 0 ? 0 : exit_code)
        + "\nresult.json: " + result_json_content
        + "\n\nRespond with JSON only:\n"
          "{\"status\": \"accept\" | \"reject\" | \"needs_test_run\", "
          "\"reason\": \"...\", "
          "\"test_code\": \"<test script or empty string>\"}"
          "\nUse \"needs_test_run\" with a test_code script to run unit tests "
          "against worker_impl.py before deciding. "
          "The test script must import worker_impl using importlib from the same "
          "directory and print \"ALL TESTS PASSED\" on success.";

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

    // Parse LLM response — strip markdown fences first.
    std::string raw_review = llm_result.value.content;
    {
      auto fence_start = raw_review.find ("```");
      if (fence_start != std::string::npos)
      {
        auto content_start = raw_review.find ('\n', fence_start);
        if (content_start != std::string::npos)
        {
          auto fence_end = raw_review.rfind ("```");
          if (fence_end != std::string::npos && fence_end > content_start)
            raw_review = raw_review.substr (content_start + 1,
                                            fence_end - content_start - 1);
        }
      }
      const auto ts = raw_review.find_first_not_of (" \t\r\n");
      const auto te = raw_review.find_last_not_of (" \t\r\n");
      if (ts != std::string::npos)
        raw_review = raw_review.substr (ts, te - ts + 1);
    }

    rapidjson::Document llm_doc;
    llm_doc.Parse (raw_review.c_str ());
    if (llm_doc.HasParseError () || !llm_doc.IsObject ())
      return make_error ("LLM response is not valid JSON: " + raw_review);

    auto status_it = llm_doc.FindMember ("status");
    if (status_it == llm_doc.MemberEnd () || !status_it->value.IsString ())
      return make_error ("LLM response missing 'status' field");

    std::string status = status_it->value.GetString ();
    if (status != "accept" && status != "reject" && status != "needs_test_run")
      return make_error ("LLM response status must be accept/reject/needs_test_run");

    std::string reason;
    auto reason_it = llm_doc.FindMember ("reason");
    if (reason_it != llm_doc.MemberEnd () && reason_it->value.IsString ())
      reason = reason_it->value.GetString ();

    // ── 9. Handle needs_test_run — run Reviewer-written tests, feed result back
    if (status == "needs_test_run")
    {
      std::string test_code;
      auto tc_it = llm_doc.FindMember ("test_code");
      if (tc_it == llm_doc.MemberEnd () || !tc_it->value.IsString ()
          || std::string (tc_it->value.GetString ()).empty ())
      {
        spdlog::warn ("[code_reviewer] needs_test_run but no test_code — "
                      "treating as reject");
        status = "reject";
        reason = "Reviewer requested test run but provided no test code";
      }
      else
      {
        test_code = tc_it->value.GetString ();

        // Write test script alongside worker_impl.py.
        const std::string test_path = scratch + "/reviewer_test.py";
        if (!write_file (test_path, test_code))
        {
          spdlog::warn ("[code_reviewer] cannot write test script");
          status = "reject";
          reason = "failed to write Reviewer test script";
        }
        else
        {
          // Run test script in same sandbox with worker_impl.py present.
          std::string test_output;
          const int test_exit = run_sandbox (
            worker_binary, test_path,
            mock_input_path,  // stdin not used by tests but sandbox needs it
            scratch, /*timeout_s=*/30, test_output);

          const bool tests_passed
            = (test_exit == 0
               && test_output.find ("ALL TESTS PASSED") != std::string::npos);

          spdlog::info ("[code_reviewer] test run exit={} passed={} output={}",
                        test_exit, tests_passed,
                        test_output.substr (0, 200));

          // Second LLM call: give Reviewer the test results for final verdict.
          const std::string test_result_summary
            = "Test exit code: " + std::to_string (test_exit)
              + "\nTest output:\n" + test_output.substr (0, 1000);

          const std::string second_prompt
            = "Requirement:\n" + description
              + "\n\nworker_impl.py source:\n```python\n" + impl_code
              + "\n```\n\nUnit test code you wrote:\n```python\n" + test_code
              + "\n```\n\n" + test_result_summary
              + "\n\nBased on the source code review AND test results, "
                "respond with JSON only:\n"
                "{\"status\": \"accept\" | \"reject\", "
                "\"reason\": \"...\", \"test_code\": \"\"}\n"
                "Remember: test passing does not guarantee correctness -- "
                "check the implementation logic too.";

          // Build second LLM request using same config as first.
          LlmRequest second_req;
          second_req.base_url = base_url_env ? base_url_env
                                             : "https://api.anthropic.com";
          second_req.api_key  = api_key_env  ? api_key_env  : "";
          second_req.model    = model_env    ? model_env    : "claude-opus-4-5";
          second_req.max_tokens = max_tokens_env
                                    ? std::atoi (max_tokens_env) : 1024;
          second_req.api_path
            = (second_req.base_url.find ("anthropic.com") != std::string::npos)
                ? "/v1/messages" : "/v1/chat/completions";
          second_req.system_prompt = system_prompt;
          second_req.user_prompt   = second_prompt;
          auto second_fut = proxy.enqueue (second_req);
          Result<LlmResponse> second_result;
          try { second_result = second_fut.get (); }
          catch (const std::exception &e)
          {
            return make_error ("second LLM call threw: "
                               + std::string (e.what ()));
          }

          if (!second_result.ok)
            return make_error ("second LLM call failed: " + second_result.error);

          std::string raw2 = second_result.value.content;
          {
            auto fs2 = raw2.find ("```");
            if (fs2 != std::string::npos)
            {
              auto cs2 = raw2.find ('\n', fs2);
              if (cs2 != std::string::npos)
              {
                auto fe2 = raw2.rfind ("```");
                if (fe2 != std::string::npos && fe2 > cs2)
                  raw2 = raw2.substr (cs2 + 1, fe2 - cs2 - 1);
              }
            }
            auto ts2 = raw2.find_first_not_of (" \t\r\n");
            auto te2 = raw2.find_last_not_of  (" \t\r\n");
            if (ts2 != std::string::npos)
              raw2 = raw2.substr (ts2, te2 - ts2 + 1);
          }

          rapidjson::Document doc2;
          doc2.Parse (raw2.c_str ());
          if (!doc2.HasParseError () && doc2.IsObject ())
          {
            auto s2 = doc2.FindMember ("status");
            auto r2 = doc2.FindMember ("reason");
            if (s2 != doc2.MemberEnd () && s2->value.IsString ())
              status = s2->value.GetString ();
            if (r2 != doc2.MemberEnd () && r2->value.IsString ())
              reason = r2->value.GetString ();
          }
          else
          {
            spdlog::warn ("[code_reviewer] second LLM response invalid JSON, "
                          "falling back to test exit code");
            status = tests_passed ? "accept" : "reject";
            reason = tests_passed ? "Tests passed (fallback decision)"
                                  : "Tests failed (fallback decision)";
          }
        }
      }
    }

    // ── 10 (was 9). Build final verdict
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
