#include "agentos/llm_client.h"
#include "agentos/llm_proxy.h"

#include <spdlog/spdlog.h>

#include <future>
#include <utility>

namespace agentos
{

  LlmClient::LlmClient (LlmProxy &proxy, const Config::Llm &cfg)
    : proxy_ (proxy), cfg_ (cfg)
  {
  }

  Result<LlmResponse> LlmClient::complete (const LlmRequest &req) const
  {
    auto fut = proxy_.enqueue (req);

    const auto t0 = std::chrono::steady_clock::now ();
    // NOTE: This is intended `delay blocking' here, we need
    //       to avoid to block in proxy.enqueue().
    //       The correct blocking point is complete()
    //       That's the reason why we use future.
    // Block until the result is available.
    auto result = fut.get ();
    const auto elapsed = std::chrono::steady_clock::now () - t0;
    spdlog::debug (
                   "[llm] model={} tokens={} elapsed={}ms", request.model,
                   request.max_tokens,
                   std::chrono::duration_cast<std::chrono::milliseconds> (elapsed).count ());
    return result;
  }

} // namespace agentos
