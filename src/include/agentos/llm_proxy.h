#ifndef AGENTOS_LLM_PROXY_H
#define AGENTOS_LLM_PROXY_H

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <chrono>
#include <cstdlib>
#include <string>

#include <httplib.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>

#include "agentos/llm_client.h" // for LlmRequest, LlmResponse
#include "agentos/types.h"      // for Result, Error

namespace agentos
{

  struct LlmWorkItem
  {
    LlmRequest request;
    std::promise<Result<LlmResponse>> promise;
  };

  /// Translate the config value 0 → auto (max(1, hardware_concurrency - 1))
  inline int resolve_concurrency (int cfg)
  {
    if (cfg > 0)
      return cfg;
    const int hw = static_cast<int> (std::thread::hardware_concurrency ());
    return std::max (1, hw - 1);
  }

  /// Single global proxy through which all LLM traffic flows.
  class LlmProxy
  {
  public:
    /// @param pool_size  number of worker threads
    /// @param timeout_s  request timeout used by every worker
    explicit LlmProxy (int pool_size, int timeout_s) : timeout_s_ (timeout_s)
    {
      workers_.reserve (pool_size);
      for (int i = 0; i < pool_size; ++i)
        {
          workers_.emplace_back (&LlmProxy::worker_loop, this);
        }
    }

    ~LlmProxy ()
    {
      {
        std::lock_guard<std::mutex> lk (mtx_);
        stop_ = true;
      }
      cv_.notify_all ();
      for (auto &t : workers_)
        {
          if (t.joinable ())
            t.join ();
      }
    }

    // Non‑copyable / non‑movable
    LlmProxy (const LlmProxy &) = delete;
    LlmProxy &operator= (const LlmProxy &) = delete;

    /// Enqueue a request and return a future that will hold the result.
    std::future<Result<LlmResponse>> enqueue (LlmRequest req);

  private:
    void worker_loop ();

    /// Perform the HTTP call, including provider selection and retries.
    static Result<LlmResponse> perform_call (const LlmRequest &req,
                                             int timeout_s);

    std::vector<std::thread> workers_;
    std::queue<LlmWorkItem> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_{false};
    int timeout_s_;
  };

} // namespace agentos

#endif // AGENTOS_LLM_PROXY_H
