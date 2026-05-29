#ifndef AGENTOS_LLM_CLIENT_H
#define AGENTOS_LLM_CLIENT_H

#include <string>
#include <string_view>

#include "agentos/config.h"   // for Config::Llm
#include "agentos/types.h"    // for Result

namespace agentos {

class LlmProxy;   // forward declaration

struct LlmRequest {
    std::string base_url;        // provider endpoint
    std::string api_key;         // credential
    std::string model;           // model identifier (e.g. "claude-opus-4-5")
    std::string system_prompt;
    std::string user_prompt;
    int         max_tokens = 1024;
};

struct LlmResponse {
    std::string content;         // raw text from the LLM
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
