#ifndef AGENTOS_LLM_CLIENT_H
#define AGENTOS_LLM_CLIENT_H

#include <optional>
#include <string>
#include <string_view>

#include "agentos/config.h"   // for Config::Llm
#include "agentos/types.h"    // for Result

namespace agentos {

class LlmProxy;   // forward declaration

/// One image attached to the user message (ADR-017 "Image input").
/// The daemon sends it inline as base64; never logged.
struct LlmImage {
    std::string media_type;      // image/jpeg | image/png | image/gif | image/webp
    std::string base64;          // standard base64 of the image bytes, no data: prefix
};

struct LlmRequest {
    std::string base_url;        // provider endpoint (scheme + host, no path)
    std::string api_key;         // credential
    std::string model;           // model identifier (e.g. "claude-opus-4-5")
    std::string user_id;         // AgentOS tenant identity for provider-side isolation
    std::string system_prompt;
    std::string user_prompt;
    int         max_tokens = 1024;
    std::string api_path = "/v1/chat/completions"; // OpenAI‑compatible path
    std::optional<LlmImage> image; // at most one image, sent with user_prompt
};

struct LlmResponse {
    std::string content;         // raw text from the LLM
    int prompt_tokens     = 0;   // input tokens consumed
    int completion_tokens = 0;   // output tokens generated
};

class LlmClient {
public:
    /// Construct with a bound proxy and the resolved LLM configuration.
    LlmClient(LlmProxy& proxy, const Config::Llm& cfg);

    /// Issue a blocking LLM call through the proxy.
    [[nodiscard]]
    Result<LlmResponse> complete(const LlmRequest& req) const;

private:
    LlmProxy&          proxy_;
    const Config::Llm& cfg_;
};

} // namespace agentos

#endif // AGENTOS_LLM_CLIENT_H
