#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "agentos/config.h"
#include "agentos/llm_client.h"
#include "agentos/llm_proxy.h"

namespace {
using namespace agentos;

// ----------------------------------------------------------------------
// FakeLlmServer  –  runs a local HTTP server for integration tests
// ----------------------------------------------------------------------
class FakeLlmServer {
public:
    FakeLlmServer() {
        svr_.Post("/v1/chat/completions",
            [](const httplib::Request& /*req*/, httplib::Response& res) {
                // Return a valid OpenAI‑style response
                res.set_content(
                    R"({"choices":[{"message":{"content":"Hello from test"}}]})",
                    "application/json");
            });

        port_ = svr_.bind_to_port("localhost", 0);
        if (port_ <= 0) {
            throw std::runtime_error("FakeLlmServer bind failed");
        }

        ready_ = true;
        thread_ = std::thread([this]() { svr_.listen_after_bind(); });
    }

    ~FakeLlmServer() {
        svr_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    int port() const { return port_; }

private:
    httplib::Server svr_;
    int port_ = 0;
    std::thread thread_;
    bool ready_ = false;
};

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

TEST(LlmProxyTest, FakeServerReturnsResponse) {
    FakeLlmServer server;
    const int port = server.port();

    LlmProxy proxy(1, 5);   // 1 worker thread, moderate timeout

    LlmRequest req;
    req.base_url      = "http://localhost:" + std::to_string(port);
    req.api_key       = "noop";
    req.model         = "test-model";
    req.system_prompt = "sysmsg";
    req.user_prompt   = "message";
    req.max_tokens    = 100;

    auto fut = proxy.enqueue(req);
    auto status = fut.wait_for(std::chrono::seconds(5));
    ASSERT_EQ(status, std::future_status::ready);

    auto res = fut.get();
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ(res.value.content, "Hello from test");
}

} // namespace
