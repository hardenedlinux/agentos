#ifndef AGENTOS_LLM_PROXY_H
#define AGENTOS_LLM_PROXY_H

#include <condition_variable>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <chrono>
#include <cstdlib>
#include <string>

#include <httplib.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/error/en.h>
#include <spdlog/spdlog.h>

#include "agentos/llm_client.h"   // for LlmRequest, LlmResponse
#include "agentos/types.h"        // for Result, Error

namespace agentos {

struct LlmWorkItem {
    LlmRequest                      request;
    std::promise<Result<LlmResponse>> promise;
};

/// Translate the config value 0 → auto (max(1, hardware_concurrency - 1))
inline int resolve_concurrency(int cfg) {
    if (cfg > 0) return cfg;
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    return std::max(1, hw - 1);
}

/// Single global proxy through which all LLM traffic flows.
class LlmProxy {
public:
    /// @param pool_size  number of worker threads
    /// @param timeout_s  request timeout used by every worker
    explicit LlmProxy(int pool_size, int timeout_s)
        : timeout_s_(timeout_s) {
        workers_.reserve(pool_size);
        for (int i = 0; i < pool_size; ++i) {
            workers_.emplace_back(&LlmProxy::worker_loop, this);
        }
    }

    ~LlmProxy() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    // Non‑copyable / non‑movable
    LlmProxy(const LlmProxy&) = delete;
    LlmProxy& operator=(const LlmProxy&) = delete;

    /// Enqueue a request and return a future that will hold the result.
    std::future<Result<LlmResponse>> enqueue(LlmRequest req) {
        LlmWorkItem item;
        item.request = std::move(req);
        auto fut = item.promise.get_future();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
        return fut;
    }

private:
    void worker_loop() {
        while (true) {
            LlmWorkItem item;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                item = std::move(queue_.front());
                queue_.pop();
            }

            // Perform the actual call (blocking, with retries)
            Result<LlmResponse> res =
                LlmProxy::perform_call(item.request, timeout_s_);
            item.promise.set_value(std::move(res));
        }
    }

    /// Perform the HTTP call, including provider selection and retries.
    static Result<LlmResponse> perform_call(const LlmRequest& req,
                                            int timeout_s) {
        const bool is_anthropic =
            req.base_url.find("anthropic.com") != std::string::npos;

        constexpr int kMaxAttempts = 3;          // initial attempt + 2 retries

        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            // ---- Serialise request -------------------------------------------------
            rapidjson::Document doc;
            doc.SetObject();
            auto& alloc = doc.GetAllocator();

            std::string path;
            httplib::Headers headers;

            if (is_anthropic) {
                // model
                rapidjson::Value model_val(req.model.c_str(),
                                          static_cast<rapidjson::SizeType>(req.model.size()),
                                          alloc);
                doc.AddMember("model", model_val, alloc);
                // max_tokens
                doc.AddMember("max_tokens", req.max_tokens, alloc);

                // system (if present)
                if (!req.system_prompt.empty()) {
                    rapidjson::Value sys(req.system_prompt.c_str(),
                                        static_cast<rapidjson::SizeType>(req.system_prompt.size()),
                                        alloc);
                    doc.AddMember("system", sys, alloc);
                }

                // messages array – a single "user" message
                rapidjson::Value msgs(rapidjson::kArrayType);
                {
                    rapidjson::Value msg(rapidjson::kObjectType);
                    rapidjson::Value role("user", 4, alloc);
                    msg.AddMember("role", role, alloc);
                    rapidjson::Value content(req.user_prompt.c_str(),
                                            static_cast<rapidjson::SizeType>(req.user_prompt.size()),
                                            alloc);
                    msg.AddMember("content", content, alloc);
                    msgs.PushBack(msg, alloc);
                }
                doc.AddMember("messages", msgs, alloc);

                // Anthropic‑specific headers
                headers = {
                    {"Content-Type", "application/json"},
                    {"x-api-key", req.api_key},
                    {"anthropic-version", "2023-06-01"}
                };
                path = "/v1/messages";
            } else {
                // OpenAI‑compatible serialisation
                // model
                rapidjson::Value model_val(req.model.c_str(),
                                          static_cast<rapidjson::SizeType>(req.model.size()),
                                          alloc);
                doc.AddMember("model", model_val, alloc);

                // messages
                rapidjson::Value messages(rapidjson::kArrayType);

                if (!req.system_prompt.empty()) {
                    rapidjson::Value sys_msg(rapidjson::kObjectType);
                    rapidjson::Value role("system", 6, alloc);
                    sys_msg.AddMember("role", role, alloc);
                    rapidjson::Value content(req.system_prompt.c_str(),
                                            static_cast<rapidjson::SizeType>(req.system_prompt.size()),
                                            alloc);
                    sys_msg.AddMember("content", content, alloc);
                    messages.PushBack(sys_msg, alloc);
                }

                {
                    rapidjson::Value user_msg(rapidjson::kObjectType);
                    rapidjson::Value role("user", 4, alloc);
                    user_msg.AddMember("role", role, alloc);
                    rapidjson::Value content(req.user_prompt.c_str(),
                                            static_cast<rapidjson::SizeType>(req.user_prompt.size()),
                                            alloc);
                    user_msg.AddMember("content", content, alloc);
                    messages.PushBack(user_msg, alloc);
                }
                doc.AddMember("messages", messages, alloc);

                doc.AddMember("max_tokens", req.max_tokens, alloc);

                headers = {
                    {"Content-Type", "application/json"},
                    {"Authorization", "Bearer " + req.api_key}
                };
                path = "/v1/chat/completions";
            }

            // Serialise to string
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            std::string body = buffer.GetString();

            // ---- HTTP transport ----------------------------------------------------
            httplib::Client cli(req.base_url);
            cli.set_connection_timeout(timeout_s, 0);
            cli.set_read_timeout(timeout_s, 0);

            auto res = cli.Post(path.c_str(), headers, body, "application/json");

            // ---- Error / retry evaluation -----------------------------------------
            if (!res) {
                std::string err_msg = httplib::to_string(res.error());
                spdlog::warn("[llm_proxy] attempt {}/{} network error: {}",
                             attempt + 1, kMaxAttempts, err_msg);
                if (attempt < (kMaxAttempts - 1)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                return Result<LlmResponse>(Error{"Network error after retries: " + err_msg},
                                           ErrorTag{});
            }

            if (res->status >= 500) {
                spdlog::warn("[llm_proxy] attempt {}/{} HTTP {}: {}",
                             attempt + 1, kMaxAttempts, res->status, res->body);
                if (attempt < (kMaxAttempts - 1)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                return Result<LlmResponse>(
                    Error{"HTTP " + std::to_string(res->status) + ": " + res->body},
                    ErrorTag{});
            }

            if (res->status >= 400 && res->status < 500) {
                spdlog::error("[llm_proxy] HTTP client error {}: {}",
                              res->status, res->body);
                return Result<LlmResponse>(
                    Error{"HTTP " + std::to_string(res->status) + ": " + res->body},
                    ErrorTag{});
            }

            // ---- Parse response ----------------------------------------------------
            rapidjson::Document resp_doc;
            rapidjson::ParseResult ok = resp_doc.Parse(res->body.c_str());
            if (!ok) {
                std::string err_msg = "Failed to parse LLM response JSON: ";
                err_msg += rapidjson::GetParseError_En(ok.Code());
                spdlog::error("[llm_proxy] {}", err_msg);
                return Result<LlmResponse>(Error{std::move(err_msg)}, ErrorTag{});
            }

            std::string content;
            if (is_anthropic) {
                // content is inside content[0].text
                if (!resp_doc.HasMember("content") || !resp_doc["content"].IsArray() ||
                    resp_doc["content"].Size() == 0) {
                    std::string err_msg = "Anthropic response missing 'content' array";
                    spdlog::error("[llm_proxy] {}", err_msg);
                    return Result<LlmResponse>(Error{std::move(err_msg)}, ErrorTag{});
                }
                const auto& first_block = resp_doc["content"][0];
                if (!first_block.HasMember("text") || !first_block["text"].IsString()) {
                    std::string err_msg = "Anthropic response block missing 'text' field";
                    spdlog::error("[llm_proxy] {}", err_msg);
                    return Result<LlmResponse>(Error{std::move(err_msg)}, ErrorTag{});
                }
                content = first_block["text"].GetString();
            } else {
                // OpenAI‑compatible: choices[0].message.content
                if (!resp_doc.HasMember("choices") || !resp_doc["choices"].IsArray() ||
                    resp_doc["choices"].Size() == 0) {
                    std::string err_msg = "LLM response missing 'choices' array";
                    spdlog::error("[llm_proxy] {}", err_msg);
                    return Result<LlmResponse>(Error{std::move(err_msg)}, ErrorTag{});
                }
                const auto& first_choice = resp_doc["choices"][0];
                if (!first_choice.HasMember("message") ||
                    !first_choice["message"].HasMember("content")) {
                    std::string err_msg = "LLM response missing 'message.content'";
                    spdlog::error("[llm_proxy] {}", err_msg);
                    return Result<LlmResponse>(Error{std::move(err_msg)}, ErrorTag{});
                }
                content = first_choice["message"]["content"].GetString();
            }

            LlmResponse response{std::move(content)};
            return Result<LlmResponse>(std::move(response));
        }

        // Should never be reached
        return Result<LlmResponse>(Error{"Unreachable"}, ErrorTag{});
    }

    std::vector<std::thread> workers_;
    std::queue<LlmWorkItem>  queue_;
    std::mutex               mtx_;
    std::condition_variable  cv_;
    bool                     stop_{false};
    int                      timeout_s_;
};

} // namespace agentos

#endif // AGENTOS_LLM_PROXY_H
