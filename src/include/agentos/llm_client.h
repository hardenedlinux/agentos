#ifndef AGENTOS_LLM_CLIENT_H
#define AGENTOS_LLM_CLIENT_H

#include <string>
#include <string_view>
#include "agentos/types.h"

namespace agentos {

struct LlmRequest {
    std::string system_prompt;
    std::string user_prompt;
    std::string model;
    int max_tokens = 1024;
};

struct LlmResponse {
    std::string content; // raw JSON string from Master's perspective
};

class LlmClient {
public:
    explicit LlmClient(std::string_view api_key, std::string_view base_url);

    [[nodiscard]]
    Result<LlmResponse> complete(const LlmRequest& req) const;

private:
    std::string api_key_;
    std::string base_url_;
};

} // namespace agentos

#endif // AGENTOS_LLM_CLIENT_H
