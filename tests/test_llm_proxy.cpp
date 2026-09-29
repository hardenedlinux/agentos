#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "agentos/config.h"
#include "agentos/llm_client.h"
#include "agentos/llm_proxy.h"

namespace {
using namespace agentos;

// ----------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------
TEST(LlmProxyTest, ResolveConcurrency) {
    // `0` -> the fixed I/O-oriented default, independent of core count.
    // LLM calls are network-bound waits, not CPU work, so the pool must
    // not be sized off hardware_concurrency() (see llm_proxy.h).
    EXPECT_EQ(resolve_concurrency(0), kDefaultLlmConcurrency);

    // explicit value is used unchanged, including values below the default
    EXPECT_EQ(resolve_concurrency(5), 5);
    EXPECT_EQ(resolve_concurrency(1), 1);
    EXPECT_EQ(resolve_concurrency(64), 64);
}

TEST(LlmProxyTest, ConstructAndDestroy) {
    // Simple smoke test: create the proxy and let it be destroyed.
    // Destructor joins all worker threads.
    LlmProxy proxy(1, 10);   // 1 thread, 10‑second timeout
    // No assertions needed – test passes if no crash / hang.
}

TEST(LlmProxyTest, EnqueueUnreachableHostReturnsError) {
    LlmProxy proxy(1, 1);   // very short timeout for faster test
    LlmRequest req;
    req.base_url      = "http://127.0.0.1:1";   // nothing listening
    req.api_key       = "dummy";
    req.model         = "gpt-3.5-turbo";
    req.system_prompt = "sys";
    req.user_prompt   = "hello";
    req.max_tokens    = 50;

    auto fut = proxy.enqueue(req);
    auto status = fut.wait_for(std::chrono::seconds(5));
    ASSERT_EQ(status, std::future_status::ready);

    auto res = fut.get();
    EXPECT_FALSE(res.ok);
    const std::string& err = res.error;
    EXPECT_TRUE(err.find("Network error") != std::string::npos ||
                err.find("Failed") != std::string::npos);
}

TEST(LlmProxyTest, DeepSeekE2E) {
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (!key) {
        GTEST_SKIP() << "DEEPSEEK_API_KEY not set";
    }

    LlmProxy proxy(1, 60);   // 1 worker thread, 60‑second timeout

    LlmRequest req;
    req.base_url      = "https://api.deepseek.com";
    req.api_key       = key;
    req.model         = "deepseek-v4-flash";      // v4 model (deepseek-chat retired 2026-07-24)
    req.api_path      = "/v1/chat/completions";   // OpenAI‑compatible endpoint
    req.system_prompt = "You are a helpful assistant.";
    req.user_prompt   = "Say hello world";
    req.max_tokens    = 30;

    auto fut = proxy.enqueue(req);
    auto status = fut.wait_for(std::chrono::seconds(60));
    ASSERT_EQ(status, std::future_status::ready);

    auto res = fut.get();
    ASSERT_TRUE(res.ok) << res.error;

    std::cout << "DeepSeek raw content: '" << res.value.content << "'\n";

    // The model must produce at least one character.
    ASSERT_FALSE(res.value.content.empty())
        << "Content was empty; check the log for possible parsing issues.";
}

// ADR-040: DeepSeek user_id mapping.
TEST(LlmProxyTest, DeepSeekUserId_DefaultUserZeroPassesThrough) {
    EXPECT_EQ(deepseek_user_id("0"), "0");
}

TEST(LlmProxyTest, DeepSeekUserId_ValidIdsPassThrough) {
    EXPECT_EQ(deepseek_user_id("alice"), "alice");
    EXPECT_EQ(deepseek_user_id("tenant_42-a"), "tenant_42-a");
    EXPECT_EQ(deepseek_user_id(std::string(512, 'a')), std::string(512, 'a'));
}

TEST(LlmProxyTest, DeepSeekUserId_InvalidIdsAreHashedStably) {
    const std::string h = deepseek_user_id("roy@example.com");
    EXPECT_EQ(h.rfind("sha256_", 0), 0u);
    EXPECT_EQ(h.size(), 7u + 64u);
    EXPECT_EQ(h, deepseek_user_id("roy@example.com"));      // stable
    EXPECT_NE(h, deepseek_user_id("roy@example.org"));      // distinct tenants
    EXPECT_EQ(h.find('@'), std::string::npos);              // raw id not leaked
    EXPECT_NE(deepseek_user_id(std::string(513, 'a')), std::string(513, 'a'));
}

// Header-bound config values are validated before any I/O, so a bad API
// key or base_url fails fast with a precise message instead of httplib's
// opaque "Invalid headers" retried as a network error.
namespace {
Result<LlmResponse> run_once(const std::string& base_url,
                             const std::string& api_key) {
    LlmProxy proxy(1, 1);
    LlmRequest req;
    req.base_url = base_url;
    req.api_key = api_key;
    req.model = "m";
    req.user_prompt = "hi";
    auto fut = proxy.enqueue(req);
    EXPECT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    return fut.get();
}
} // namespace

TEST(LlmProxyTest, EmptyApiKey_FailsFastWithClearError) {
    auto res = run_once("https://api.deepseek.com", "");
    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.error.find("API key is empty"), std::string::npos) << res.error;
}

TEST(LlmProxyTest, WhitespaceOnlyApiKey_TreatedAsEmpty) {
    auto res = run_once("https://api.deepseek.com", " \n");
    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.error.find("API key is empty"), std::string::npos) << res.error;
}

TEST(LlmProxyTest, ApiKeyWithEmbeddedControlChar_FailsWithoutLeakingKey) {
    auto res = run_once("https://api.deepseek.com", "sk-abc\ndef");
    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.error.find("invalid character (byte 0x0a at position 6)"),
              std::string::npos) << res.error;
    EXPECT_EQ(res.error.find("sk-abc"), std::string::npos);
}

TEST(LlmProxyTest, TrailingNewlineInKeyAndUrl_IsTrimmedAndRequestIsSent) {
    // Trimmed values pass validation; the request then reaches the network
    // layer (nothing listening) instead of failing with "Invalid headers".
    auto res = run_once("http://127.0.0.1:1\n", "dummy\n");
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error.find("Invalid"), std::string::npos) << res.error;
    EXPECT_EQ(res.error.find("API key"), std::string::npos) << res.error;
}

TEST(LlmProxyTest, BaseUrlWithPath_Rejected) {
    auto res = run_once("https://api.deepseek.com/v1", "k");
    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.error.find("base_url is invalid"), std::string::npos) << res.error;
}

} // namespace
