#include <string>
#include <future>
#include <cstdlib>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include "agentos/llm_proxy.h"
#include "agentos/llm_client.h"
#include "agentos/types.h"
#include "agentos/forge/code_reviewer.h"

namespace agentos::forge {

std::string code_reviewer(const std::string& input_json) {
    // Parse input JSON
    rapidjson::Document input;
    input.Parse(input_json.c_str());
    if (input.HasParseError()) {
        rapidjson::Document err;
        err.SetObject();
        err.AddMember("status", "error", err.GetAllocator());
        err.AddMember("reason", "Failed to parse input JSON", err.GetAllocator());
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        err.Accept(w);
        return buf.GetString();
    }

    // Extract fields
    std::string task_id = input.HasMember("task_id") && input["task_id"].IsString()
                              ? input["task_id"].GetString()
                              : "unknown";
    std::string code = input.HasMember("code") && input["code"].IsString()
                           ? input["code"].GetString()
                           : "";
    std::string requirement = input.HasMember("requirement") && input["requirement"].IsString()
                                  ? input["requirement"].GetString()
                                  : "";
    std::string language = input.HasMember("language") && input["language"].IsString()
                               ? input["language"].GetString()
                               : "python";

    if (code.empty()) {
        rapidjson::Document err;
        err.SetObject();
        err.AddMember("status", "error", err.GetAllocator());
        err.AddMember("reason", "Missing 'code' field", err.GetAllocator());
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        err.Accept(w);
        return buf.GetString();
    }

    // Read LLM configuration from environment
    const char* base_url_env = std::getenv("AGENTOS_LLM_BASE_URL");
    const char* api_key_env  = std::getenv("AGENTOS_LLM_API_KEY");
    const char* model_env    = std::getenv("AGENTOS_LLM_MODEL");

    std::string base_url = base_url_env ? base_url_env : "https://api.anthropic.com";
    std::string api_key  = api_key_env  ? api_key_env  : "";
    std::string model    = model_env    ? model_env    : "claude-opus-4-5";

    if (api_key.empty()) {
        rapidjson::Document err;
        err.SetObject();
        err.AddMember("status", "error", err.GetAllocator());
        err.AddMember("reason", "AGENTOS_LLM_API_KEY environment variable not set", err.GetAllocator());
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        err.Accept(w);
        return buf.GetString();
    }

    // Determine API path based on provider
    bool is_anthropic = base_url.find("anthropic.com") != std::string::npos;
    std::string api_path = is_anthropic ? "/v1/messages" : "/v1/chat/completions";

    // Build LLM request
    LlmRequest llm_req;
    llm_req.base_url = base_url;
    llm_req.api_key  = api_key;
    llm_req.model    = model;
    llm_req.system_prompt = "You are a code reviewer. Review the following " + language +
                            " code for correctness, security, and style. "
                            "Return a JSON object with fields 'status' (either 'accept' or 'reject'), "
                            "'reason' (string), and 'suggestions' (string).";
    llm_req.user_prompt = "Requirement: " + requirement + "\n\nCode:\n" + code;
    llm_req.max_tokens  = 2048;
    llm_req.api_path    = api_path;

    // Create a static proxy (initialised once)
    static LlmProxy proxy(1, 30);

    // Enqueue request and wait for result
    auto fut = proxy.enqueue(llm_req);
    auto result = fut.get();

    if (!result.ok) {
        rapidjson::Document err;
        err.SetObject();
        err.AddMember("status", "error", err.GetAllocator());
        err.AddMember("reason", result.error.message, err.GetAllocator());
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        err.Accept(w);
        return buf.GetString();
    }

    std::string llm_output = result.value.content;

    // Try to parse the LLM output as JSON; if it fails, wrap it in an error
    rapidjson::Document llm_json;
    llm_json.Parse(llm_output.c_str());
    if (llm_json.HasParseError() || !llm_json.HasMember("status")) {
        // Fallback: return the raw LLM output as a reason
        rapidjson::Document out;
        out.SetObject();
        out.AddMember("status", "error", out.GetAllocator());
        out.AddMember("reason", rapidjson::Value(llm_output.c_str(), out.GetAllocator()).Move(), out.GetAllocator());
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        out.Accept(w);
        return buf.GetString();
    }

    // Build output JSON with the same structure as the stub
    rapidjson::Document out;
    out.SetObject();
    out.AddMember("status", rapidjson::Value(llm_json["status"].GetString(), out.GetAllocator()).Move(), out.GetAllocator());
    out.AddMember("reason", rapidjson::Value(llm_json["reason"].GetString(), out.GetAllocator()).Move(), out.GetAllocator());
    if (llm_json.HasMember("suggestions")) {
        out.AddMember("suggestions", rapidjson::Value(llm_json["suggestions"].GetString(), out.GetAllocator()).Move(), out.GetAllocator());
    } else {
        out.AddMember("suggestions", "", out.GetAllocator());
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    out.Accept(w);
    return buf.GetString();
}

} // namespace agentos::forge
