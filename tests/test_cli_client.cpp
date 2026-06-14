#include <gtest/gtest.h>

#include <future>
#include <thread>
#include <chrono>
#include <string>
#include <cstring>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <zmq.hpp>

#include "agentos/cli_client.h"

// ---- helpers ----------------------------------------------------------------

// Bind a TCP ROUTER on a random port and return the bound address.
static std::string bind_router(zmq::socket_t& sock)
{
    sock.bind("tcp://127.0.0.1:0");
    char addr[256];
    size_t addr_len = sizeof(addr);
    auto ep = sock.get(zmq::sockopt::last_endpoint);
    std::strncpy(addr, ep.c_str(), sizeof(addr) - 1);
    addr[sizeof(addr) - 1] = '\0';
    return std::string(addr);
}

// ROUTER recv/reply helpers.
// Frame layout from DEALER: [identity][payload]  (no empty delimiter)
static void recv_frames(zmq::socket_t& sock,
                        zmq::message_t& ident,
                        zmq::message_t& payload)
{
    [[maybe_unused]] auto r1 = sock.recv(ident,   zmq::recv_flags::none);
    [[maybe_unused]] auto r2 = sock.recv(payload, zmq::recv_flags::none);
}

static void send_reply(zmq::socket_t& sock,
                       zmq::message_t& ident,
                       const rapidjson::Document& resp)
{
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    resp.Accept(w);

    zmq::message_t id_copy(ident.data(), ident.size());
    sock.send(id_copy, zmq::send_flags::sndmore);
    zmq::message_t body(buf.GetSize());
    std::memcpy(body.data(), buf.GetString(), buf.GetSize());
    sock.send(body, zmq::send_flags::none);
}

static rapidjson::Document parse_request(zmq::message_t& payload)
{
    rapidjson::Document req;
    req.Parse(static_cast<const char*>(payload.data()), payload.size());
    return req;
}

// ---- tests ------------------------------------------------------------------

TEST(CliClientTest, Timeout)
{
    // Server binds but never replies.
    zmq::context_t srv_ctx(1);
    zmq::socket_t  srv_sock(srv_ctx, zmq::socket_type::router);
    std::string addr = bind_router(srv_sock);

    // Signal that server is bound before client connects.
    std::promise<void> ready;
    std::future<void>  ready_fut = ready.get_future();

    std::promise<void> done;
    std::future<void>  done_fut = done.get_future();

    std::thread server([&] {
        ready.set_value();
        // Wait until client test finishes before closing socket.
        done_fut.wait();
    });

    ready_fut.wait();

    {
        agentos::cli::CliClient client(addr, "dummy_key", 150);
        rapidjson::Document params(rapidjson::kObjectType);
        EXPECT_THROW(
            (void)client.send("test.method", std::move(params)),
            agentos::cli::CliError
        );
    }

    done.set_value();
    server.join();
}

TEST(CliClientTest, JsonRpcError)
{
    zmq::context_t srv_ctx(1);
    zmq::socket_t  srv_sock(srv_ctx, zmq::socket_type::router);
    std::string addr = bind_router(srv_sock);

    std::promise<void> ready;
    std::future<void>  ready_fut = ready.get_future();

    std::thread server([&] {
        ready.set_value();

        zmq::message_t ident, payload;
        recv_frames(srv_sock, ident, payload);

        rapidjson::Document req = parse_request(payload);

        rapidjson::Document resp(rapidjson::kObjectType);
        auto& alloc = resp.GetAllocator();
        resp.AddMember("jsonrpc", "2.0", alloc);
        resp.AddMember("id", rapidjson::Value(req["id"], alloc).Move(), alloc);
        rapidjson::Value err(rapidjson::kObjectType);
        err.AddMember("code", -32020, alloc);
        err.AddMember("message", "not found", alloc);
        resp.AddMember("error", err, alloc);

        send_reply(srv_sock, ident, resp);
    });

    ready_fut.wait();

    agentos::cli::CliClient client(addr, "dummy_key", 2000);
    rapidjson::Document params(rapidjson::kObjectType);
    try {
        (void)client.send("test.method", std::move(params));
        FAIL() << "expected CliError";
    } catch (const agentos::cli::CliError& e) {
        EXPECT_NE(std::string(e.what()).find("not found"), std::string::npos);
    }

    server.join();
}

TEST(CliClientTest, JsonRpcSuccess)
{
    zmq::context_t srv_ctx(1);
    zmq::socket_t  srv_sock(srv_ctx, zmq::socket_type::router);
    std::string addr = bind_router(srv_sock);

    std::promise<void> ready;
    std::future<void>  ready_fut = ready.get_future();

    std::thread server([&] {
        ready.set_value();

        zmq::message_t ident, payload;
        recv_frames(srv_sock, ident, payload);

        rapidjson::Document req = parse_request(payload);

        rapidjson::Document resp(rapidjson::kObjectType);
        auto& alloc = resp.GetAllocator();
        resp.AddMember("jsonrpc", "2.0", alloc);
        resp.AddMember("id", rapidjson::Value(req["id"], alloc).Move(), alloc);
        rapidjson::Value result(rapidjson::kObjectType);
        result.AddMember("ok", true, alloc);
        resp.AddMember("result", result, alloc);

        send_reply(srv_sock, ident, resp);
    });

    ready_fut.wait();

    agentos::cli::CliClient client(addr, "dummy_key", 2000);
    rapidjson::Document params(rapidjson::kObjectType);
    rapidjson::Document result = client.send("test.method", std::move(params));
    ASSERT_TRUE(result.HasMember("ok"));
    EXPECT_TRUE(result["ok"].GetBool());

    server.join();
}
