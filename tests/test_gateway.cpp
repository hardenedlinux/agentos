/**
 * test_gateway.cpp — ADR-020 Gateway unit tests
 *
 * ZMQ ROUTER/DEALER frame layout:
 *
 *   Inbound (DEALER → ROUTER):
 *     DEALER sends:    [payload]              (1 frame, no empty delimiter)
 *     ROUTER receives: [identity][payload]    (2 frames)
 *     gateway.cpp strips identity, passes payload to ForwardFn.
 *
 *   Outbound (inproc PUSH → ROUTER → DEALER):
 *     Sender pushes:   [identity][payload]    (2 frames)
 *     ROUTER strips identity, routes remaining frame.
 *     DEALER receives: [payload]              (1 frame)
 *
 * Note: the empty delimiter frame is a REQ/REP convention, NOT used by
 * DEALER sockets. DEALER sends raw frames without any envelope.
 */
#include "agentos/gateway.h"
#include "agentos/home_init.h"

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// TempHome
// ---------------------------------------------------------------------------
struct TempHome
{
  fs::path dir;
  TempHome ()
  {
    char tmpl[] = "/tmp/agentos_gw_test_XXXXXX";
    if (mkdtemp (tmpl) == nullptr)
      throw std::runtime_error ("mkdtemp failed");
    dir = tmpl;
    fs::create_directories (dir / "run");
    setenv ("AGENTOS_HOME", dir.c_str (), 1);
  }
  ~TempHome ()
  {
    unsetenv ("AGENTOS_HOME");
    fs::remove_all (dir);
  }
  std::string sock_uri () const
  {
    return "ipc://" + (dir / "run" / "agentos.sock").string ();
  }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class GatewayTest : public ::testing::Test
{
protected:
  TempHome home_;
  zmq::context_t ctx_{1};

  // Build and start a Gateway.
  //
  // `received` and `first_msg_promise` must outlive the returned Gateway
  // (caller's responsibility — they live in the test body).
  //
  // mu and fulfilled are heap-allocated so the Gateway thread can safely
  // access them after make_gateway() returns.
  std::unique_ptr<agentos::Gateway>
  make_gateway (std::vector<agentos::GatewayInbound> &received,
                std::promise<void> &first_msg_promise)
  {
    auto mu = std::make_shared<std::mutex> ();
    auto fulfilled = std::make_shared<bool> (false);

    auto fn = [&received, &first_msg_promise, mu,
               fulfilled] (agentos::GatewayInbound msg)
    {
      received.push_back (std::move (msg));
      std::lock_guard lk (*mu);
      if (!*fulfilled)
      {
        *fulfilled = true;
        first_msg_promise.set_value ();
      }
    };

    auto gw = std::make_unique<agentos::Gateway> (ctx_, std::move (fn));
    gw->bind_inproc ();
    gw->start ();
    return gw;
  }

  // Connect a DEALER with a fixed identity string.
  // Uses the C API to set ZMQ_ROUTING_ID to avoid cppzmq 4.10 BINARY
  // sockopt resolution ambiguity.
  zmq::socket_t make_dealer (const std::string &identity)
  {
    zmq::socket_t dealer (ctx_, zmq::socket_type::dealer);
    int rc = zmq_setsockopt (dealer.handle (), ZMQ_ROUTING_ID, identity.data (),
                             identity.size ());
    if (rc != 0)
      throw zmq::error_t ();
    dealer.connect (home_.sock_uri ());
    std::this_thread::sleep_for (50ms);
    return dealer;
  }

  // Receive a single frame with a timeout. Returns nullopt on timeout.
  std::optional<std::string> recv_frame (zmq::socket_t &sock,
                                         std::chrono::milliseconds timeout = 2s)
  {
    zmq::pollitem_t items[] = {{static_cast<void *> (sock), 0, ZMQ_POLLIN, 0}};
    if (zmq::poll (items, 1, timeout) == 0)
      return std::nullopt;
    zmq::message_t msg;
    if (!sock.recv (msg, zmq::recv_flags::dontwait).has_value ())
      return std::nullopt;
    return msg.to_string ();
  }
};

// ---------------------------------------------------------------------------
// Test 1: inbound payload and identity reach the ForwardFn correctly
// ---------------------------------------------------------------------------
TEST_F (GatewayTest,
        InboundMessage_ForwardedToCallback_WithCorrectIdentityAndPayload)
{
  std::vector<agentos::GatewayInbound> received;
  std::promise<void> arrived;
  auto fut = arrived.get_future ();

  auto gw = make_gateway (received, arrived);
  zmq::socket_t dealer = make_dealer ("client-A");

  const std::string payload
    = R"({"jsonrpc":"2.0","id":"1","method":"job.submit","params":{}})";
  zmq::message_t msg (payload.data (), payload.size ());
  dealer.send (std::move (msg), zmq::send_flags::none);

  ASSERT_EQ (fut.wait_for (2s), std::future_status::ready)
    << "Gateway did not forward inbound message within 2s";
  gw->stop ();

  ASSERT_EQ (received.size (), 1u);
  EXPECT_EQ (received[0].identity, "client-A");
  EXPECT_EQ (received[0].message, payload);
}

// ---------------------------------------------------------------------------
// Test 2: outbound message pushed to inproc reaches the DEALER
//
// Sender pushes [identity][payload] (no empty delimiter).
// DEALER receives [payload] (1 frame).
// ---------------------------------------------------------------------------
TEST_F (GatewayTest, OutboundMessage_PushedToInproc_ReachesCorrectClient)
{
  std::promise<void> unused_promise;
  std::vector<agentos::GatewayInbound> unused_received;
  auto gw = make_gateway (unused_received, unused_promise);

  const char *client_id = "client-B";
  zmq::socket_t dealer = make_dealer (client_id);

  const std::string out_payload
    = R"({"jsonrpc":"2.0","method":"system.heartbeat","params":{}})";

  {
    zmq::socket_t pusher (ctx_, zmq::socket_type::push);
    pusher.connect ("inproc://gateway-out");

    zmq::message_t id_frame (client_id, std::strlen (client_id));
    pusher.send (std::move (id_frame), zmq::send_flags::sndmore);

    zmq::message_t payload_frame (out_payload.data (), out_payload.size ());
    pusher.send (std::move (payload_frame), zmq::send_flags::none);
  }

  auto received = recv_frame (dealer);
  ASSERT_TRUE (received.has_value ()) << "Timed out waiting for payload frame";
  EXPECT_EQ (*received, out_payload);

  gw->stop ();
}

// ---------------------------------------------------------------------------
// Test 3: outbound is drained before inbound is processed (ADR-020)
// ---------------------------------------------------------------------------
TEST_F (GatewayTest, OutboundDrainedBeforeInbound)
{
  std::vector<std::string> order;
  auto order_mu = std::make_shared<std::mutex> ();
  std::promise<void> inbound_arrived;
  auto fut = inbound_arrived.get_future ();
  auto promise_set = std::make_shared<bool> (false);

  auto fn =
    [&order, &inbound_arrived, order_mu, promise_set] (agentos::GatewayInbound)
  {
    std::lock_guard lk (*order_mu);
    order.push_back ("inbound");
    if (!*promise_set)
    {
      *promise_set = true;
      inbound_arrived.set_value ();
    }
  };

  auto gw = std::make_unique<agentos::Gateway> (ctx_, fn);
  gw->bind_inproc ();
  gw->start ();

  const char *client_id = "client-C";
  zmq::socket_t dealer = make_dealer (client_id);

  // Push outbound first: [identity][payload]
  {
    zmq::socket_t pusher (ctx_, zmq::socket_type::push);
    pusher.connect ("inproc://gateway-out");

    zmq::message_t id_frame (client_id, std::strlen (client_id));
    pusher.send (std::move (id_frame), zmq::send_flags::sndmore);
    const std::string out = "heartbeat";
    zmq::message_t pf (out.data (), out.size ());
    pusher.send (std::move (pf), zmq::send_flags::none);
  }

  // Dealer receives [payload]; record "outbound".
  {
    auto payload_frame = recv_frame (dealer);
    ASSERT_TRUE (payload_frame.has_value ())
      << "Timed out waiting for outbound payload";
    std::lock_guard lk (*order_mu);
    order.push_back ("outbound");
  }

  // Now send inbound.
  const std::string payload
    = R"({"jsonrpc":"2.0","id":"2","method":"job.list","params":{}})";
  zmq::message_t msg (payload.data (), payload.size ());
  dealer.send (std::move (msg), zmq::send_flags::none);

  ASSERT_EQ (fut.wait_for (2s), std::future_status::ready)
    << "Inbound message not received within 2s";
  gw->stop ();

  ASSERT_GE (order.size (), 2u);
  EXPECT_EQ (order[0], "outbound");
  EXPECT_EQ (order[1], "inbound");
}

// ---------------------------------------------------------------------------
// Test 4: stop() is idempotent
// ---------------------------------------------------------------------------
TEST_F (GatewayTest, Stop_CalledTwice_DoesNotCrash)
{
  std::promise<void> unused;
  std::vector<agentos::GatewayInbound> unused_received;
  auto gw = make_gateway (unused_received, unused);
  gw->stop ();
  EXPECT_NO_THROW (gw->stop ());
}
