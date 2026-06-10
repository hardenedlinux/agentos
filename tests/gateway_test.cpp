#include "agentos/gateway.h"   // must be included with private→public trick
#define private public           // allow access to zmq::socket_t members
#undef private

#include "agentos/central.h"        // mock central needs full definition? we don't need.
#include "agentos/home_init.h"      // agentos_home() override via env
#include <zmq.hpp>
#include <spdlog/spdlog.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// -----------------------------------------------------------------------
// Minimal mock central – records forwarded GatewayInbound messages
// -----------------------------------------------------------------------
struct MockCentral
{
    std::vector<agentos::GatewayInbound> inbound_msgs;

    template <typename Tag>
    void send (agentos::GatewayInbound msg)
    {
        inbound_msgs.push_back (std::move (msg));
    }
};

// -----------------------------------------------------------------------
// Helper to create a temporary directory and set AGENTOS_HOME
// -----------------------------------------------------------------------
struct TempAgentOsHome
{
    std::filesystem::path dir;
    TempAgentOsHome ()
    {
        dir = std::filesystem::temp_directory_path ()
              / "agentos_gateway_test_XXXXXX";
        auto s = dir.string ();
        if (mkdtemp (s.data ()) == nullptr)
            throw std::runtime_error ("mkdtemp");
        dir = s;
        {
            auto r = dir / "run";
            std::filesystem::create_directories (r);
        }
        // override environment for the duration of this helper
        setenv ("AGENTOS_HOME", dir.c_str (), 1);
    }
    ~TempAgentOsHome ()
    {
        unsetenv ("AGENTOS_HOME");
        std::filesystem::remove_all (dir);
    }
    std::string socket_path () const
    {
        return "ipc://" + (dir / "run" / "agentos.sock").string ();
    }
};

// -----------------------------------------------------------------------
// Utility: send a single-part message on a DEALER socket
// -----------------------------------------------------------------------
static void dealer_send (zmq::socket_t &dealer, const std::string &payload)
{
    zmq::message_t msg (payload.data (), payload.size ());
    dealer.send (std::move (msg), zmq::send_flags::none);
}

// -----------------------------------------------------------------------
// Utility: receive a message from a DEALER with a short timeout
// -----------------------------------------------------------------------
static bool dealer_recv (zmq::socket_t &dealer, std::string &out_payload,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds (2000))
{
    zmq::pollitem_t items[] = {
        { dealer, 0, ZMQ_POLLIN, 0 }
    };
    if (zmq::poll (items, 1, timeout) == 0)
        return false;
    zmq::message_t msg;
    if (!dealer.recv (msg, zmq::recv_flags::dontwait))
        return false;
    out_payload = msg.to_string ();
    return true;
}

// -----------------------------------------------------------------------
// Basic inbound test
// -----------------------------------------------------------------------
static void test_inbound_forwarding ()
{
    spdlog::info ("=== test_inbound_forwarding ===");
    TempAgentOsHome home;
    zmq::context_t ctx;

    MockCentral central;
    agentos::Gateway gw (ctx, central);

    // The gateway's internal PULL socket must be bound before start.
    // Use the #define private trick to access the member.
    gw.inproc_pull_.bind ("inproc://gateway-out");

    gw.start ();

    // connect a DEALER that will act as an external client
    zmq::socket_t dealer (ctx, zmq::socket_type::dealer);
    dealer.setsockopt (ZMQ_IDENTITY, "test-client-1", 13);
    dealer.connect (home.socket_path ());

    // give the gateway thread a moment to enter poll()
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    // send one inbound JSON‑RPC message
    const std::string json_payload = R"({"jsonrpc":"2.0","id":"42","method":"submit"})";
    dealer_send (dealer, json_payload);

    // wait for gateway to process, then stop
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    gw.stop ();

    assert (!central.inbound_msgs.empty ());
    assert (central.inbound_msgs[0].identity == "test-client-1");
    assert (central.inbound_msgs[0].message == json_payload);

    spdlog::info ("test_inbound_forwarding PASSED");
}

// -----------------------------------------------------------------------
// Outbound forwarding test
// -----------------------------------------------------------------------
static void test_outbound_forwarding ()
{
    spdlog::info ("=== test_outbound_forwarding ===");
    TempAgentOsHome home;
    zmq::context_t ctx;

    MockCentral central;
    agentos::Gateway gw (ctx, central);

    // bind the internal PULL socket
    gw.inproc_pull_.bind ("inproc://gateway-out");

    gw.start ();

    // connect a dealer with a well‑known identity so the gateway can route
    // outbound messages to it.
    zmq::socket_t dealer (ctx, zmq::socket_type::dealer);
    const char *client_id = "client-out-1";
    dealer.setsockopt (ZMQ_IDENTITY, client_id, std::strlen (client_id));
    dealer.connect (home.socket_path ());

    // let the gateway see the new connection
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    // push an outbound multipart message onto inproc://gateway-out
    {
        zmq::socket_t pusher (ctx, zmq::socket_type::push);
        pusher.connect ("inproc://gateway-out");

        // frame 1 – identity (route to matching client)
        zmq::message_t id_frame (client_id, std::strlen (client_id));
        pusher.send (std::move (id_frame), zmq::send_flags::sndmore);

        // frame 2 – empty delimiter (ROUTER convention)
        zmq::message_t delim_frame (nullptr, 0);
        pusher.send (std::move (delim_frame), zmq::send_flags::sndmore);

        // frame 3 – payload
        const std::string out_payload = "outbound-heartbeat";
        zmq::message_t payload_frame (out_payload.data (), out_payload.size ());
        pusher.send (std::move (payload_frame), zmq::send_flags::none);
    }

    // wait for the dealer to receive the forwarded payload
    std::string received;
    bool ok = dealer_recv (dealer, received, std::chrono::milliseconds (3000));
    gw.stop ();

    assert (ok);
    assert (received == "outbound-heartbeat");

    spdlog::info ("test_outbound_forwarding PASSED");
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------
int main ()
{
    spdlog::set_level (spdlog::level::info);
    test_inbound_forwarding ();
    test_outbound_forwarding ();
    spdlog::info ("All gateway tests PASSED");
    return 0;
}
