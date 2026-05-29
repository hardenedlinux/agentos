#include "agentos/llm_client.h"
#include "agentos/llm_proxy.h"

#include <spdlog/spdlog.h>

#include <future>
#include <utility>

namespace agentos {

LlmClient::LlmClient(LlmProxy& proxy, const Config::Llm& cfg)
    : proxy_(proxy), cfg_(cfg) {}

Result<LlmResponse> LlmClient::complete(const LlmRequest& req) const {
    auto fut = proxy_.enqueue(req);
    // Block until the result is available
    return fut.get();
}

} // namespace agentos
