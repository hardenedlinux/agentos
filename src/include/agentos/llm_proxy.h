#pragma once
/**
 * agentos/llm_proxy.h
 *
 * LlmProxy — single global component through which all outbound LLM traffic
 * flows (ADR-017).
 *
 * One instance lives for the lifetime of the daemon, created at startup before
 * any subsystem that issues LLM calls. Non-copyable, non-movable.
 *
 * All heavy includes (httplib, rapidjson) are confined to llm_proxy.cpp;
 * this header exposes only the minimal interface.
 */
#include "agentos/llm_client.h" // LlmRequest, LlmResponse
#include "agentos/types.h"      // Result, Error

#include <algorithm>
#include <condition_variable>
#include <future>
#include <mutex>
#include <queue>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>

namespace agentos
{

  struct LlmWorkItem
  {
    LlmRequest request;
    std::promise<Result<LlmResponse>> promise;
  };

  /// Translate config value 0 → auto (max(1, hardware_concurrency - 1)).
  inline int resolve_concurrency (int cfg)
  {
    if (cfg > 0)
      return cfg;
    const int hw = static_cast<int> (std::thread::hardware_concurrency ());
    return std::max (1, hw - 1);
  }

  class LlmProxy
  {
  public:
    /// @param pool_size  number of worker threads
    /// @param timeout_s  HTTP request timeout applied to every worker
    explicit LlmProxy (int pool_size, int timeout_s) : timeout_s_ (timeout_s)
    {
      workers_.reserve (pool_size);
      for (int i = 0; i < pool_size; ++i)
        workers_.emplace_back (&LlmProxy::worker_loop, this);
    }

    ~LlmProxy ()
    {
      {
        std::lock_guard<std::mutex> lk (mtx_);
        stop_ = true;
      }
      cv_.notify_all ();
      for (auto &t : workers_)
        if (t.joinable ())
          t.join ();
    }

    LlmProxy (const LlmProxy &) = delete;
    LlmProxy &operator= (const LlmProxy &) = delete;
    LlmProxy (LlmProxy &&) = delete;
    LlmProxy &operator= (LlmProxy &&) = delete;

    /// Enqueue a request; returns a future that resolves when the call
    /// completes. Caller blocks on fut.get() — see LlmClient::complete().
    std::future<Result<LlmResponse>> enqueue (LlmRequest req);

  private:
    void worker_loop ();

    /// Perform one HTTP round-trip (with retries). Called from worker_loop
    /// only. Implementation lives entirely in llm_proxy.cpp; httplib is never
    /// visible in this header.
    Result<LlmResponse> perform_call (const LlmRequest &req, int timeout_s);

    std::vector<std::thread> workers_;
    std::queue<LlmWorkItem> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_{false};
    int timeout_s_;
  };

} // namespace agentos
