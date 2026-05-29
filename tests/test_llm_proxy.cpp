#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
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
    // `0`  →  max(1, hardware_concurrency - 1)
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    const int expected = std::max(1, hw - 1);
    EXPECT_EQ(resolve_concurrency(0), expected);

    // explicit value is used unchanged
    EXPECT_EQ(resolve_concurrency(5), 5);
    EXPECT_EQ(resolve_concurrency(1), 1);
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
    req.model         = "deepseek-v4-flash";
    req.api_path      = "/chat/completions";          // DeepSeek‑specific path
    req.system_prompt = "Answer with exactly one word.";
    req.user_prompt   = "What is the color of the sky?";
    req.max_tokens    = 16;

    auto fut = proxy.enqueue(req);
    auto status = fut.wait_for(std::chrono::seconds(60));
    ASSERT_EQ(status, std::future_status::ready);

    auto res = fut.get();
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_FALSE(res.value.content.empty());
}

} // namespace
