#pragma once
/**
 * agentos/gateway.h
 *
 * ADR-020: Gateway thread — pure I/O for the external ZMQ ROUTER socket.
 * Owns agentos.sock (ROUTER) and holds the PULL end of inproc://gateway-out.
 *
 * The inproc PULL socket is bound by calling bind_inproc() before start().
 * In production this is done by Central (ADR-024); in tests it is called
 * directly.
 *
 * The forward_fn callback is invoked for every inbound message. In production
 * Central supplies a lambda that calls central.send<Orchestrator>(msg). In
 * tests a capturing lambda stores messages for assertion.
 */

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <zmq.hpp>

#include "agentos/types.h"

namespace agentos
{

  class Gateway
  {
  public:
    using ForwardFn = std::function<void (GatewayInbound)>;

    // zmq_ctx   — shared ZMQ context (owned by Central / test harness)
    // forward_fn — called on the Gateway thread for each inbound message;
    //              must be thread-safe w.r.t. whatever it captures.
    Gateway (zmq::context_t &zmq_ctx, ForwardFn forward_fn);
    ~Gateway ();

    // Non-copyable / non-movable
    Gateway (const Gateway &) = delete;
    Gateway &operator= (const Gateway &) = delete;

    // Bind inproc://gateway-out (PULL side).
    // Must be called before start() and before any thread pushes to that
    // endpoint. Called by Central in production (ADR-024); called directly in
    // tests.
    void bind_inproc ();

    // Bind the external ROUTER socket and start the I/O thread.
    // bind_inproc() must have been called first.
    void start ();

    // Signal the I/O thread to exit and join it.
    void stop ();

  private:
    void run ();

    std::atomic<bool> running_{false};
    std::thread thread_;

    zmq::context_t &zmq_ctx_;
    zmq::socket_t agentos_sock_; // ZMQ_ROUTER bound to agentos.sock
    zmq::socket_t inproc_pull_;  // ZMQ_PULL   bound to inproc://gateway-out
    ForwardFn forward_fn_;
  };

} // namespace agentos
