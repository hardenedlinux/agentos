#ifndef AGENTOS_LLM_PROXY_H
#define AGENTOS_LLM_PROXY_H

#include <condition_variable>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "agentos/llm_client.h"   // for LlmRequest, LlmResponse
#include "agentos/types.h"        // for Result, Error

namespace agentos {

struct LlmWorkItem {
    LlmRequest                      request;
    std::promise<Result<LlmResponse>> promise;
};

/// Translate the config value 0 → auto (max(1, hardware_concurrency - 1))
int resolve_concurrency(int cfg);

/// Single global proxy through which all LLM traffic flows.
class LlmProxy {
public:
    /// @param pool_size  number of worker threads
    /// @param timeout_s  request timeout used by every worker
    explicit LlmProxy(int pool_size, int timeout_s);
    ~LlmProxy();

    // Non‑copyable / non‑movable
    LlmProxy(const LlmProxy&) = delete;
    LlmProxy& operator=(const LlmProxy&) = delete;

    /// Enqueue a request and return a future that will hold the result.
    std::future<Result<LlmResponse>> enqueue(LlmRequest req);

private:
    void worker_loop();

    /// Perform the HTTP call, including provider selection and retries.
    static Result<LlmResponse> perform_call(const LlmRequest& req,
                                            int timeout_s);

    std::vector<std::thread> workers_;
    std::queue<LlmWorkItem>  queue_;
    std::mutex               mtx_;
    std::condition_variable  cv_;
    bool                     stop_{false};
    int                      timeout_s_;
};

} // namespace agentos

#endif // AGENTOS_LLM_PROXY_H
