/**
 * agentos/llm/llm_proxy.cpp
 *
 * ADR-012: LLM calls are synchronous, non-streaming.
 * ADR-017: Single LlmProxy instance; all LLM traffic flows through it.
 *
 * HTTP transport: cpp-httplib with OpenSSL support.
 * Provider selection: derived from base_url (anthropic.com → Anthropic API;
 *                     everything else → OpenAI-compatible).
 */
#include "agentos/llm_proxy.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <thread>

#include <httplib.h>
#include <openssl/crypto.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>

namespace agentos
{

  // ─────────────────────────────────────────────────────────────────────────────
  // Internal helpers
  // ─────────────────────────────────────────────────────────────────────────────

  namespace
  {

    // Strip scheme prefix so httplib::SSLClient receives only the host.
    std::string strip_scheme (const std::string &url)
    {
      if (url.size () > 8 && url.substr (0, 8) == "https://")
        return url.substr (8);
      if (url.size () > 7 && url.substr (0, 7) == "http://")
        return url.substr (7);
      return url;
    }

    // ADR-017 (DeepSeek-specific 429 handling).
    //
    // DeepSeek's 429 is a concurrency-slot rejection (per-account /
    // per-user_id concurrent-connection ceiling), not a fixed-window
    // token/request quota -- a slot frees the instant some other in-flight
    // request on the account completes. That makes the wait typically
    // short and self-clearing, so it is worth retrying the same request in
    // place rather than immediately failing the step and letting ADR-031's
    // max_step_retries re-invoke the whole Adviser from scratch.
    //
    // Bounded by elapsed wall time (`max_wait_s`), not attempt count -- an
    // attempt-count budget has no natural relationship to "keep waiting
    // while the account is at its concurrency ceiling right now". This is
    // deliberately separate from the generic network/5xx retry loop in
    // perform_call, which stays attempt-counted (kMaxAttempts).
    //
    // This scoped to DeepSeek `base_url`s only -- other OpenAI-compatible
    // providers' 429s continue to fall under the ordinary "4xx: no retry"
    // rule until a provider's own documented semantics justify the same
    // treatment. Detection is by HTTP status code only, never by parsing
    // the response body: DeepSeek (like other providers) may return error
    // detail as a nested object ({"error": {"message": "..."}}) rather
    // than a string, and status-code detection sidesteps that entirely.
    //
    // Retries synchronously inside the pool thread that drew the request --
    // consistent with LlmProxy's existing blocking model -- rather than
    // requeuing the item to the back of the shared queue. Accepted
    // trade-off: Suite jobs are already split into discrete steps
    // (ADR-031), so one stalled invocation only holds one thread for one
    // step of one job, not the rest of that job's pipeline or other jobs.
    httplib::Result retry_deepseek_429 (
      const std::function<httplib::Result ()> &send, int max_wait_s)
    {
      const auto deadline = std::chrono::steady_clock::now ()
                           + std::chrono::seconds (max_wait_s);
      int backoff_s = 2;
      httplib::Result res;

      std::random_device rd;
      std::mt19937 gen (rd ());
      std::uniform_int_distribution<int> jitter_ms (0, 1000);

      while (std::chrono::steady_clock::now () < deadline)
      {
        spdlog::warn (
          "[llm_proxy] DeepSeek 429 (concurrency limit); retrying in {}s",
          backoff_s);
        std::this_thread::sleep_for (std::chrono::seconds (backoff_s)
                                     + std::chrono::milliseconds (
                                         jitter_ms (gen)));
        backoff_s = std::min (backoff_s * 2, 30);

        res = send ();
        if (res && res->status != 429)
          return res; // success, or a different error -- let the caller
                       // fall back into its normal status handling

        // still 429, or a network hiccup mid-backoff: keep retrying
        // against the same deadline
      }

      spdlog::error (
        "[llm_proxy] DeepSeek 429: rate_limit_max_wait_s ({}) exceeded",
        max_wait_s);
      return res; // last result (429, or empty on network failure)
    }

    // Serialise an Anthropic-format request body.
    std::string build_anthropic_body (const LlmRequest &req)
    {
      rapidjson::Document doc;
      doc.SetObject ();
      auto &alloc = doc.GetAllocator ();

      doc.AddMember (
        "model",
        rapidjson::Value (req.model.c_str (),
                          static_cast<rapidjson::SizeType> (req.model.size ()),
                          alloc),
        alloc);
      doc.AddMember ("max_tokens", req.max_tokens, alloc);

      if (!req.system_prompt.empty ())
        doc.AddMember ("system",
                       rapidjson::Value (req.system_prompt.c_str (),
                                         static_cast<rapidjson::SizeType> (
                                           req.system_prompt.size ()),
                                         alloc),
                       alloc);

      rapidjson::Value msgs (rapidjson::kArrayType);
      {
        rapidjson::Value msg (rapidjson::kObjectType);
        msg.AddMember ("role", rapidjson::Value ("user", 4, alloc), alloc);
        msg.AddMember (
          "content",
          rapidjson::Value (
            req.user_prompt.c_str (),
            static_cast<rapidjson::SizeType> (req.user_prompt.size ()), alloc),
          alloc);
        msgs.PushBack (msg, alloc);
      }
      doc.AddMember ("messages", msgs, alloc);

      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      doc.Accept (w);
      return buf.GetString ();
    }

    // Serialise an OpenAI-compatible request body.
    std::string build_openai_body (const LlmRequest &req, bool is_deepseek)
    {
      rapidjson::Document doc;
      doc.SetObject ();
      auto &alloc = doc.GetAllocator ();

      doc.AddMember (
        "model",
        rapidjson::Value (req.model.c_str (),
                          static_cast<rapidjson::SizeType> (req.model.size ()),
                          alloc),
        alloc);

      rapidjson::Value messages (rapidjson::kArrayType);

      if (!req.system_prompt.empty ())
      {
        rapidjson::Value sys (rapidjson::kObjectType);
        sys.AddMember ("role", rapidjson::Value ("system", 6, alloc), alloc);
        sys.AddMember ("content",
                       rapidjson::Value (req.system_prompt.c_str (),
                                         static_cast<rapidjson::SizeType> (
                                           req.system_prompt.size ()),
                                         alloc),
                       alloc);
        messages.PushBack (sys, alloc);
      }

      {
        rapidjson::Value user (rapidjson::kObjectType);
        user.AddMember ("role", rapidjson::Value ("user", 4, alloc), alloc);
        user.AddMember (
          "content",
          rapidjson::Value (
            req.user_prompt.c_str (),
            static_cast<rapidjson::SizeType> (req.user_prompt.size ()), alloc),
          alloc);
        messages.PushBack (user, alloc);
      }

      doc.AddMember ("messages", messages, alloc);
      doc.AddMember ("max_tokens", req.max_tokens, alloc);

      // DeepSeek uses user_id as its provider-side multi-tenant isolation key.
      // Never send this field to unrelated OpenAI-compatible providers: the
      // AgentOS tenant identity is internal, while user_id has provider-
      // specific semantics.
      if (is_deepseek && !req.user_id.empty ())
        doc.AddMember (
          "user_id",
          rapidjson::Value (req.user_id.c_str (),
                            static_cast<rapidjson::SizeType> (req.user_id.size ()),
                            alloc),
          alloc);

      // Disable DeepSeek chain-of-thought reasoning by default (ADR future).
      // Reasoning adds significant latency and token cost with no benefit for
      // structured JSON outputs. Will be made configurable per-Adviser later.
      // Both fields for compatibility across DeepSeek API versions.
      rapidjson::Value thinking (rapidjson::kObjectType);
      thinking.AddMember ("type",
                          rapidjson::Value ("disabled", alloc).Move (),
                          alloc);
      doc.AddMember ("thinking", thinking.Move (), alloc);
      doc.AddMember ("enable_thinking", false, alloc);

      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      doc.Accept (w);
      return buf.GetString ();
    }

    // Extract content string from a parsed Anthropic response.
    Result<std::string> extract_anthropic_content (const rapidjson::Document &d)
    {
      if (!d.HasMember ("content") || !d["content"].IsArray ()
          || d["content"].Size () == 0)
        return Result<std::string> (
          Error{"Anthropic response missing 'content' array"}, ErrorTag{});

      const auto &block = d["content"][0];
      if (!block.HasMember ("text") || !block["text"].IsString ())
        return Result<std::string> (
          Error{"Anthropic response block missing 'text'"}, ErrorTag{});

      return Result<std::string> (std::string (block["text"].GetString ()));
    }

    // Extract content string from a parsed OpenAI-compatible response.
    Result<std::string> extract_openai_content (const rapidjson::Document &d)
    {
      if (!d.HasMember ("choices") || !d["choices"].IsArray ()
          || d["choices"].Size () == 0)
        return Result<std::string> (
          Error{"LLM response missing 'choices' array"}, ErrorTag{});

      const auto &choice = d["choices"][0];
      if (!choice.HasMember ("message")
          || !choice["message"].HasMember ("content"))
        return Result<std::string> (
          Error{"LLM response missing 'message.content'"}, ErrorTag{});

      const auto &c = choice["message"]["content"];
      if (!c.IsString ())
        return Result<std::string> (
          Error{"LLM message.content is not a string"}, ErrorTag{});

      return Result<std::string> (std::string (c.GetString ()));
    }

    // Parse token usage from OpenAI-compatible response (best-effort).
    void extract_openai_usage (const rapidjson::Document &d,
                               int &prompt_tokens, int &completion_tokens)
    {
      prompt_tokens = 0;
      completion_tokens = 0;
      if (!d.HasMember ("usage") || !d["usage"].IsObject ())
        return;
      const auto &u = d["usage"];
      if (u.HasMember ("prompt_tokens") && u["prompt_tokens"].IsInt ())
        prompt_tokens = u["prompt_tokens"].GetInt ();
      if (u.HasMember ("completion_tokens") && u["completion_tokens"].IsInt ())
        completion_tokens = u["completion_tokens"].GetInt ();
    }

  } // namespace

  // ─────────────────────────────────────────────────────────────────────────────
  // perform_call — one HTTP round-trip, called from worker_loop
  // ─────────────────────────────────────────────────────────────────────────────

  Result<LlmResponse> LlmProxy::perform_call (const LlmRequest &req,
                                              int timeout_s,
                                              int rate_limit_max_wait_s)
  {
    const bool is_anthropic
      = req.base_url.find ("anthropic.com") != std::string::npos;
    // ADR-017 (DeepSeek-specific 429 handling) -- see retry_deepseek_429
    // above for why this provider gets a carve-out from the generic
    // "4xx: no retry" rule.
    const bool is_deepseek
      = req.base_url.find ("deepseek.com") != std::string::npos;

    const std::string host = strip_scheme (req.base_url);
    const std::string path = is_anthropic ? "/v1/messages" : req.api_path;
    const std::string body
      = is_anthropic ? build_anthropic_body (req)
                     : build_openai_body (req, is_deepseek);

    httplib::Headers headers;
    if (is_anthropic)
      headers = {{"Content-Type", "application/json"},
                 {"x-api-key", req.api_key},
                 {"anthropic-version", "2023-06-01"}};
    else
      headers = {{"Content-Type", "application/json"},
                 {"Authorization", "Bearer " + req.api_key}};

    // CA bundle: AGENTOS_CA_CERT_PATH env var → system default
    const char *ca_env = std::getenv ("AGENTOS_CA_CERT_PATH");
    const std::string ca_path
      = (ca_env && *ca_env) ? ca_env : "/etc/ssl/certs/ca-certificates.crt";

    // host must outlive this lambda; it is a local std::string in scope
    // for the remainder of the function. Constructs a fresh SSLClient per
    // call, matching the previous per-attempt construction -- shared now
    // by both the outer attempt loop and retry_deepseek_429's inner loop.
    auto send_once = [&] () -> httplib::Result {
      httplib::SSLClient cli (host, 443);
      cli.set_ca_cert_path (ca_path);
      cli.set_connection_timeout (timeout_s, 0);
      cli.set_read_timeout (timeout_s, 0);
      return cli.Post (path, headers, body, "application/json");
    };

    constexpr int kMaxAttempts = 3;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
      {
        spdlog::info ("[llm_proxy] attempt {}/{} → {}{} user_id={}",
                      attempt + 1, kMaxAttempts, host, path,
                      req.user_id.empty () ? "<none>" : req.user_id);

        auto res = send_once ();

        // ADR-017 (DeepSeek-specific 429 handling): intercept before the
        // generic 4xx branch below. Does not consume this iteration's
        // slot in kMaxAttempts -- see retry_deepseek_429's own bound.
        if (res && res->status == 429 && is_deepseek)
          res = retry_deepseek_429 (send_once, rate_limit_max_wait_s);

        if (!res)
      {
        const std::string err = httplib::to_string (res.error ());
        spdlog::warn ("[llm_proxy] network error: {}", err);
        if (attempt < kMaxAttempts - 1)
        {
          std::this_thread::sleep_for (std::chrono::milliseconds (500));
          continue;
        }
        return Result<LlmResponse> (Error{"Network error: " + err}, ErrorTag{});
      }

      if (res->status >= 500)
      {
        spdlog::warn ("[llm_proxy] HTTP {}: {}", res->status, res->body);
        if (attempt < kMaxAttempts - 1)
        {
          std::this_thread::sleep_for (std::chrono::milliseconds (500));
          continue;
        }
        return Result<LlmResponse> (
          Error{"HTTP " + std::to_string (res->status) + ": " + res->body},
          ErrorTag{});
      }

      if (res->status >= 400)
      {
        spdlog::error ("[llm_proxy] HTTP client error {}: {}", res->status,
                       res->body);
        return Result<LlmResponse> (
          Error{"HTTP " + std::to_string (res->status) + ": " + res->body},
          ErrorTag{});
      }

      // Parse response
      rapidjson::Document d;
      d.Parse (res->body.c_str ());
      if (d.HasParseError ())
        return Result<LlmResponse> (
          Error{std::string ("JSON parse error: ")
                + rapidjson::GetParseError_En (d.GetParseError ())},
          ErrorTag{});

      auto content_result = is_anthropic ? extract_anthropic_content (d)
                                         : extract_openai_content (d);

      if (!content_result.ok)
        return Result<LlmResponse> (Error{content_result.error}, ErrorTag{});

      LlmResponse resp;
      resp.content = std::move (content_result.value);

      // Extract token usage (OpenAI-compatible only; Anthropic TBD).
      if (!is_anthropic)
        extract_openai_usage (d, resp.prompt_tokens, resp.completion_tokens);

      spdlog::debug ("[llm_proxy] response ({} chars, tokens: {}p+{}c)",
                     resp.content.size (),
                     resp.prompt_tokens, resp.completion_tokens);
      return Result<LlmResponse> (std::move (resp));
    }

    return Result<LlmResponse> (Error{"Max attempts exceeded"}, ErrorTag{});
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // LlmProxy public interface
  // ─────────────────────────────────────────────────────────────────────────────

  std::future<Result<LlmResponse>> LlmProxy::enqueue (LlmRequest req)
  {
    LlmWorkItem item;
    item.request = std::move (req);
    auto fut = item.promise.get_future ();
    {
      std::lock_guard<std::mutex> lk (mtx_);
      queue_.push (std::move (item));
    }
    cv_.notify_one ();
    return fut;
  }

  void LlmProxy::worker_loop ()
  {
    // Clean up OpenSSL thread-local state when this thread exits.
    struct SslCleanup
    {
      ~SslCleanup ()
      {
        OPENSSL_thread_stop ();
      }
    } ssl_cleanup;

    while (true)
    {
      LlmWorkItem item;
      {
        std::unique_lock<std::mutex> lk (mtx_);
        cv_.wait (lk, [this] { return stop_ || !queue_.empty (); });
        if (stop_ && queue_.empty ())
          return;
        item = std::move (queue_.front ());
        queue_.pop ();
      }

      try
      {
        auto res = perform_call (item.request, timeout_s_,
                                 rate_limit_max_wait_s_);
        item.promise.set_value (std::move (res));
      }
      catch (const std::exception &e)
      {
        spdlog::error ("[llm_proxy] exception in worker: {}", e.what ());
        item.promise.set_exception (std::current_exception ());
      }
      catch (...)
      {
        spdlog::error ("[llm_proxy] unknown exception in worker");
        item.promise.set_exception (std::current_exception ());
      }
    }
  }

} // namespace agentos
