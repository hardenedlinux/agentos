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
    std::string build_openai_body (const LlmRequest &req)
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
                                              int timeout_s)
  {
    const bool is_anthropic
      = req.base_url.find ("anthropic.com") != std::string::npos;

    const std::string host = strip_scheme (req.base_url);
    const std::string path = is_anthropic ? "/v1/messages" : req.api_path;
    const std::string body
      = is_anthropic ? build_anthropic_body (req) : build_openai_body (req);

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

    constexpr int kMaxAttempts = 3;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
      {
        // Construct a fresh SSLClient each attempt.
        // host must outlive the SSLClient; it is a local std::string.
        spdlog::info ("[llm_proxy] connecting to host='{}' path='{}'", host,
                      path);
        httplib::SSLClient cli (host, 443);
        cli.set_ca_cert_path (ca_path);
        cli.set_connection_timeout (timeout_s, 0);
        cli.set_read_timeout (timeout_s, 0);

        spdlog::info ("[llm_proxy] attempt {}/{} → {}{}", attempt + 1,
                      kMaxAttempts, host, path);

        auto res = cli.Post (path, headers, body, "application/json");

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
        auto res = perform_call (item.request, timeout_s_);
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
