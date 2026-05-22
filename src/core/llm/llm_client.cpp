#include "agentos/llm_client.h"

#include <httplib.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/error/en.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace agentos {

LlmClient::LlmClient(std::string_view api_key, std::string_view base_url)
    : api_key_(api_key), base_url_(base_url) {}

Result<LlmResponse> LlmClient::complete(const LlmRequest& req) const {
    // Build JSON request body
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    // model
    rapidjson::Value model_val;
    model_val.SetString(req.model.c_str(), static_cast<rapidjson::SizeType>(req.model.size()), alloc);
    doc.AddMember("model", model_val, alloc);

    // messages
    rapidjson::Value messages(rapidjson::kArrayType);

    // system message
    if (!req.system_prompt.empty()) {
        rapidjson::Value sys_msg(rapidjson::kObjectType);
        rapidjson::Value role_val;
        role_val.SetString("system", 6, alloc);
        sys_msg.AddMember("role", role_val, alloc);
        rapidjson::Value content_val;
        content_val.SetString(req.system_prompt.c_str(),
                              static_cast<rapidjson::SizeType>(req.system_prompt.size()), alloc);
        sys_msg.AddMember("content", content_val, alloc);
        messages.PushBack(sys_msg, alloc);
    }

    // user message
    rapidjson::Value user_msg(rapidjson::kObjectType);
    rapidjson::Value role_val;
    role_val.SetString("user", 4, alloc);
    user_msg.AddMember("role", role_val, alloc);
    rapidjson::Value content_val;
    content_val.SetString(req.user_prompt.c_str(),
                          static_cast<rapidjson::SizeType>(req.user_prompt.size()), alloc);
    user_msg.AddMember("content", content_val, alloc);
    messages.PushBack(user_msg, alloc);

    doc.AddMember("messages", messages, alloc);

    // max_tokens
    doc.AddMember("max_tokens", req.max_tokens, alloc);

    // Serialize to string
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    std::string body = buffer.GetString();

    // Prepare HTTP client
    httplib::Client cli(base_url_);
    cli.set_connection_timeout(30, 0);
    cli.set_read_timeout(120, 0);

    // Set headers
    httplib::Headers headers = {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + api_key_}
    };

    // Send POST request
    auto res = cli.Post("/v1/chat/completions", headers, body, "application/json");
    if (!res) {
        auto err = res.error();
        std::string err_msg = httplib::to_string(err);
        spdlog::error("[llm_client] HTTP request failed: {}", err_msg);
        return Result<LlmResponse>(Error{err_msg});
    }

    if (res->status != 200) {
        std::string err_msg = "HTTP status " + std::to_string(res->status) + ": " + res->body;
        spdlog::error("[llm_client] {}", err_msg);
        return Result<LlmResponse>(Error{err_msg});
    }

    // Parse JSON response
    rapidjson::Document resp_doc;
    rapidjson::ParseResult ok = resp_doc.Parse(res->body.c_str());
    if (!ok) {
        std::string err_msg = "Failed to parse LLM response JSON: ";
        err_msg += rapidjson::GetParseError_En(ok.Code());
        spdlog::error("[llm_client] {}", err_msg);
        return Result<LlmResponse>(Error{err_msg});
    }

    // Extract content from choices[0].message.content
    if (!resp_doc.HasMember("choices") || !resp_doc["choices"].IsArray() ||
        resp_doc["choices"].Size() == 0) {
        std::string err_msg = "LLM response missing 'choices' array";
        spdlog::error("[llm_client] {}", err_msg);
        return Result<LlmResponse>(Error{err_msg});
    }

    const auto& first_choice = resp_doc["choices"][0];
    if (!first_choice.HasMember("message") || !first_choice["message"].HasMember("content")) {
        std::string err_msg = "LLM response missing 'message.content'";
        spdlog::error("[llm_client] {}", err_msg);
        return Result<LlmResponse>(Error{err_msg});
    }

    std::string content = first_choice["message"]["content"].GetString();
    LlmResponse response{std::move(content)};
    return Result<LlmResponse>(std::move(response));
}

} // namespace agentos
