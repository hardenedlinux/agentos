#include "agentos/forge/code_reviewer.h"
#include "agentos/error_utils.h"
#include "agentos/llm_client.h"
#include "agentos/llm_proxy.h"
#include "agentos/types.h"
#include "agentos/home_init.h"
#include <cstdlib>
#include <future>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <vector>
#include <map>
#include <algorithm>
#include <regex>
#include <chrono>
#include <thread>
#include <filesystem>
#include <system_error>
#include <cassert>
#include <memory>
#include <functional>
#include <stdexcept>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <sys/prctl.h>

namespace agentos::forge
{

  // ---------------------------------------------------------------------------
  // Helper: read entire file into string
  // ---------------------------------------------------------------------------
  static std::string read_file(const std::string &path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
  }

  // ---------------------------------------------------------------------------
  // Helper: write string to file
  // ---------------------------------------------------------------------------
  static bool write_file(const std::string &path, const std::string &content) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) return false;
    ofs << content;
    return ofs.good();
  }

  // ---------------------------------------------------------------------------
  // Helper: generate mock input JSON from input_schema
  // ---------------------------------------------------------------------------
  static std::string generate_mock_input(const rapidjson::Value &schema,
                                          const std::string &scratch_dir) {
    rapidjson::Document mock;
    mock.SetObject();
    auto &alloc = mock.GetAllocator();

    for (auto it = schema.MemberBegin(); it != schema.MemberEnd(); ++it) {
      const std::string field_name = it->name.GetString();
      const rapidjson::Value &field_def = it->value;
      if (!field_def.IsObject()) continue;
      auto type_it = field_def.FindMember("type");
      if (type_it == field_def.MemberEnd() || !type_it->value.IsString()) continue;
      std::string type = type_it->value.GetString();

      if (type == "string") {
        mock.AddMember(rapidjson::Value(field_name, alloc).Move(),
                       rapidjson::Value("").Move(), alloc);
      } else if (type == "int") {
        mock.AddMember(rapidjson::Value(field_name, alloc).Move(),
                       rapidjson::Value(0).Move(), alloc);
      } else if (type == "path") {
        // create a temp path under scratch_dir
        std::string tmp_path = scratch_dir + "/" + field_name + "_mock";
        mock.AddMember(rapidjson::Value(field_name, alloc).Move(),
                       rapidjson::Value(tmp_path.c_str(), alloc).Move(), alloc);
      } else {
        // fallback: empty string
        mock.AddMember(rapidjson::Value(field_name, alloc).Move(),
                       rapidjson::Value("").Move(), alloc);
      }
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    mock.Accept(writer);
    return buf.GetString();
  }

  // ---------------------------------------------------------------------------
  // Helper: validate output JSON against output_schema
  // ---------------------------------------------------------------------------
  static std::string validate_output_schema(const rapidjson::Value &output,
                                             const rapidjson::Value &schema) {
    for (auto it = schema.MemberBegin(); it != schema.MemberEnd(); ++it) {
      const std::string field_name = it->name.GetString();
      const rapidjson::Value &field_def = it->value;
      if (!field_def.IsObject()) continue;
      auto type_it = field_def.FindMember("type");
      if (type_it == field_def.MemberEnd() || !type_it->value.IsString()) continue;
      std::string expected_type = type_it->value.GetString();

      auto out_it = output.FindMember(field_name.c_str());
      if (out_it == output.MemberEnd()) {
        return "missing field '" + field_name + "' in output";
      }
      const rapidjson::Value &val = out_it->value;
      if (expected_type == "string" && !val.IsString()) {
        return "field '" + field_name + "' expected string, got different type";
      } else if (expected_type == "int" && !val.IsInt()) {
        return "field '" + field_name + "' expected int, got different type";
      } else if (expected_type == "path" && !val.IsString()) {
        return "field '" + field_name + "' expected path (string), got different type";
      }
    }
    return {}; // empty means ok
  }

  // ---------------------------------------------------------------------------
  // Helper: run sandbox (fork + exec with basic isolation)
  // ---------------------------------------------------------------------------
  static int run_sandbox(const std::string &worker_binary,
                         const std::string &mock_input_path,
                         const std::string &scratch_dir,
                         int timeout_seconds,
                         std::string &output_content) {
    // Create output file path
    std::string output_path = scratch_dir + "/sandbox_output.json";

    pid_t pid = fork();
    if (pid == -1) {
      return -1; // fork failed
    }

    if (pid == 0) {
      // Child process
      // Basic sandbox: unshare mount namespace and network namespace
      if (unshare(CLONE_NEWNS | CLONE_NEWNET) == -1) {
        _exit(127);
      }

      // Redirect stdout/stderr to output file
      int fd = open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd == -1) {
        _exit(127);
      }
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      close(fd);

      // Execute worker binary with mock input path as argument
      execlp(worker_binary.c_str(), worker_binary.c_str(),
             mock_input_path.c_str(), nullptr);
      // If execlp fails
      _exit(127);
    }

    // Parent process
    int status;
    auto start = std::chrono::steady_clock::now();
    while (true) {
      pid_t ret = waitpid(pid, &status, WNOHANG);
      if (ret == pid) {
        // child exited
        break;
      }
      if (ret == -1) {
        // error
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return -2;
      }
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
      if (elapsed >= timeout_seconds) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return -3; // timeout
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (WIFEXITED(status)) {
      int exit_code = WEXITSTATUS(status);
      // Read output file
      output_content = read_file(output_path);
      return exit_code;
    } else {
      // child terminated by signal
      return -4;
    }
  }

  // ---------------------------------------------------------------------------
  // Main code_reviewer implementation
  // ---------------------------------------------------------------------------
  std::string code_reviewer (const std::string &input_json)
  {
    // 1. Parse input JSON
    rapidjson::Document doc;
    doc.Parse(input_json.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
      return make_error("invalid input JSON");
    }

    // Extract mandatory fields
    auto task_id_it = doc.FindMember("task_id");
    if (task_id_it == doc.MemberEnd() || !task_id_it->value.IsString()) {
      return make_error("missing or invalid 'task_id'");
    }
    std::string task_id = task_id_it->value.GetString();

    auto forge_job_id_it = doc.FindMember("forge_job_id");
    if (forge_job_id_it == doc.MemberEnd() || !forge_job_id_it->value.IsString()) {
      return make_error("missing or invalid 'forge_job_id'");
    }
    std::string forge_job_id = forge_job_id_it->value.GetString();

    auto requirement_it = doc.FindMember("requirement");
    if (requirement_it == doc.MemberEnd() || !requirement_it->value.IsObject()) {
      return make_error("missing or invalid 'requirement'");
    }
    const rapidjson::Value &requirement = requirement_it->value;

    auto writer_output_it = doc.FindMember("writer_output");
    if (writer_output_it == doc.MemberEnd() || !writer_output_it->value.IsObject()) {
      return make_error("missing or invalid 'writer_output'");
    }
    const rapidjson::Value &writer_output = writer_output_it->value;

    // Extract writer_output fields
    auto code_it = writer_output.FindMember("code");
    if (code_it == writer_output.MemberEnd() || !code_it->value.IsString()) {
      return make_error("missing or invalid 'writer_output.code'");
    }
    std::string code = code_it->value.GetString();

    auto language_it = writer_output.FindMember("language");
    if (language_it == writer_output.MemberEnd() || !language_it->value.IsString()) {
      return make_error("missing or invalid 'writer_output.language'");
    }
    std::string language = language_it->value.GetString();

    auto capability_it = writer_output.FindMember("capability");
    if (capability_it == writer_output.MemberEnd() || !capability_it->value.IsObject()) {
      return make_error("missing or invalid 'writer_output.capability'");
    }
    const rapidjson::Value &capability = capability_it->value;

    auto understanding_it = writer_output.FindMember("understanding");
    if (understanding_it == writer_output.MemberEnd() || !understanding_it->value.IsString()) {
      return make_error("missing or invalid 'writer_output.understanding'");
    }
    std::string understanding = understanding_it->value.GetString();

    // 2. Layer pre-check (ADR-009 Layer B, ADR-019)
    {
      auto net_it = capability.FindMember("network");
      if (net_it != capability.MemberEnd() && net_it->value.IsBool() && net_it->value.GetBool()) {
        return make_error("policy violation: network access declared");
      }
      auto exec_it = capability.FindMember("exec");
      if (exec_it != capability.MemberEnd() && exec_it->value.IsBool() && exec_it->value.GetBool()) {
        return make_error("policy violation: exec access declared");
      }
    }

    // 3. Determine scratch directory
    std::filesystem::path base = agentos_home();
    std::filesystem::path scratch_dir = base / "forge" / forge_job_id;
    std::error_code ec;
    std::filesystem::create_directories(scratch_dir, ec);
    if (ec) {
      return make_error("failed to create scratch directory: " + ec.message());
    }
    std::string scratch_dir_str = scratch_dir.string();

    // 4. Write code to file
    std::string ext;
    if (language == "python") ext = ".py";
    else if (language == "guile") ext = ".scm";
    else ext = ".txt";
    std::string code_path = scratch_dir_str + "/sandbox_probe" + ext;
    if (!write_file(code_path, code)) {
      return make_error("failed to write code file");
    }

    // 5. Construct mock input
    auto input_schema_it = requirement.FindMember("input_schema");
    rapidjson::Value emptySchema;
    emptySchema.SetObject();
    const rapidjson::Value &input_schema =
        (input_schema_it != requirement.MemberEnd() && input_schema_it->value.IsObject())
            ? input_schema_it->value
            : emptySchema;
    std::string mock_input = generate_mock_input(input_schema, scratch_dir_str);
    std::string mock_input_path = scratch_dir_str + "/mock_input.json";
    if (!write_file(mock_input_path, mock_input)) {
      return make_error("failed to write mock input file");
    }

    // 6. Sandbox execution (simplified)
    // Determine worker binary path (placeholder: use python3 or guile)
    std::string worker_binary;
    if (language == "python") {
      worker_binary = "/usr/bin/python3";
    } else if (language == "guile") {
      worker_binary = "/usr/bin/guile";
    } else {
      return make_error("unsupported language: " + language);
    }

    int timeout_seconds = 30; // default timeout
    std::string sandbox_output;
    int exit_code = run_sandbox(worker_binary, mock_input_path,
                                scratch_dir_str, timeout_seconds,
                                sandbox_output);
    if (exit_code == -1) {
      return make_error("sandbox fork failed");
    } else if (exit_code == -2) {
      return make_error("sandbox waitpid error");
    } else if (exit_code == -3) {
      return make_error("sandbox timeout");
    } else if (exit_code == -4) {
      return make_error("sandbox child terminated by signal");
    }

    // 7. Capability honesty check (placeholder: only check exit code)
    if (exit_code != 0) {
      return make_error("worker exited with code " + std::to_string(exit_code));
    }

    // 8. Validate sandbox output against output_schema
    auto output_schema_it = requirement.FindMember("output_schema");
    const rapidjson::Value &output_schema =
        (output_schema_it != requirement.MemberEnd() && output_schema_it->value.IsObject())
            ? output_schema_it->value
            : emptySchema;

    rapidjson::Document output_doc;
    output_doc.Parse(sandbox_output.c_str());
    if (output_doc.HasParseError() || !output_doc.IsObject()) {
      return make_error("sandbox output is not valid JSON");
    }
    std::string validation_error = validate_output_schema(output_doc, output_schema);
    if (!validation_error.empty()) {
      return make_error("output schema validation failed: " + validation_error);
    }

    // 9. LLM review
    // Read system prompt from skill.md
    const char* skill_path_env = std::getenv("AGENTOS_ADVISER_SKILL_PATH");
    std::string system_prompt;
    if (skill_path_env) {
      system_prompt = read_file(skill_path_env);
    }
    if (system_prompt.empty()) {
      system_prompt = "You are a code reviewer. Evaluate the following code for functional correctness.";
    }

    // Read LLM config from environment
    const char* base_url = std::getenv("AGENTOS_ADVISER_BASE_URL");
    const char* api_key = std::getenv("AGENTOS_ADVISER_API_KEY");
    const char* model = std::getenv("AGENTOS_ADVISER_MODEL");
    const char* max_tokens_str = std::getenv("AGENTOS_ADVISER_MAX_TOKENS");
    const char* timeout_str = std::getenv("AGENTOS_ADVISER_TIMEOUT_S");

    if (!api_key || std::strlen(api_key) == 0) {
      return make_error("LLM API key not set (AGENTOS_ADVISER_API_KEY)");
    }

    int max_tokens = 1024;
    if (max_tokens_str) max_tokens = std::atoi(max_tokens_str);
    int llm_timeout = 60;
    if (timeout_str) llm_timeout = std::atoi(timeout_str);

    // Build user prompt
    std::string user_prompt;
    {
      auto desc_it = requirement.FindMember("description");
      std::string description = (desc_it != requirement.MemberEnd() && desc_it->value.IsString())
                                    ? desc_it->value.GetString()
                                    : "";
      user_prompt = "Requirement description:\n" + description + "\n\n";
      user_prompt += "Writer's understanding:\n" + understanding + "\n\n";
      user_prompt += "Code:\n```\n" + code + "\n```\n\n";
      user_prompt += "Sandbox execution summary:\n";
      user_prompt += "  exit code: " + std::to_string(exit_code) + "\n";
      user_prompt += "  output: " + sandbox_output + "\n";
      user_prompt += "  capability check: passed\n\n";
      user_prompt += "Please respond with JSON: {\"status\": \"accept\"|\"reject\", \"reason\": \"...\"}";
    }

    // Construct LlmProxy (local, not static)
    LlmProxy proxy(1, llm_timeout); // pool_size=1, timeout_s=llm_timeout

    LlmRequest req;
    req.model = model ? model : "default";
    req.system_prompt = system_prompt;
    req.user_prompt = user_prompt;
    req.max_tokens = max_tokens;
    req.temperature = 0.0;

    // Enqueue request and wait for result
    std::promise<LlmResponse> promise;
    auto future = promise.get_future();

    proxy.enqueue(req, [&promise](const LlmResponse &resp) {
      promise.set_value(resp);
    });

    LlmResponse llm_resp;
    try {
      llm_resp = future.get();
    } catch (const std::exception &e) {
      return make_error("LLM call failed: " + std::string(e.what()));
    }

    // Parse LLM response JSON
    rapidjson::Document llm_doc;
    llm_doc.Parse(llm_resp.content.c_str());
    if (llm_doc.HasParseError() || !llm_doc.IsObject()) {
      return make_error("LLM response is not valid JSON");
    }
    auto status_it = llm_doc.FindMember("status");
    if (status_it == llm_doc.MemberEnd() || !status_it->value.IsString()) {
      return make_error("LLM response missing 'status' field");
    }
    std::string status = status_it->value.GetString();
    if (status != "accept" && status != "reject") {
      return make_error("LLM response status must be 'accept' or 'reject'");
    }

    auto reason_it = llm_doc.FindMember("reason");
    std::string reason;
    if (reason_it != llm_doc.MemberEnd() && reason_it->value.IsString()) {
      reason = reason_it->value.GetString();
    }

    // 10. Build final verdict JSON
    rapidjson::Document verdict;
    verdict.SetObject();
    auto &alloc = verdict.GetAllocator();
    verdict.AddMember("task_id", rapidjson::Value(task_id.c_str(), alloc).Move(), alloc);
    verdict.AddMember("status", rapidjson::Value(status.c_str(), alloc).Move(), alloc);
    verdict.AddMember("reason", rapidjson::Value(reason.c_str(), alloc).Move(), alloc);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    verdict.Accept(writer);
    return buf.GetString();
  }
} // namespace agentos::forge
