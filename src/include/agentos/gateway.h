#pragma once
/**
 * agentos/gateway.h
 *
 * ADR-020: Gateway thread — pure I/O for the external ZMQ ROUTER socket.
 * Owns agentos.sock and inproc://gateway-out (PULL). No business logic.
 */

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <zmq.hpp>

#include "agentos/types.h"

namespace agentos {

class Central; // forward (ADR-024)

class Gateway {
public:
    Gateway(zmq::context_t &zmq_ctx, Central &central);
    ~Gateway();

    // Non-copyable / non-movable
    Gateway(const Gateway &) = delete;
    Gateway &operator=(const Gateway &) = delete;

    // Start the I/O loop in a dedicated thread.
    // Binds the ROUTER and inproc PULL sockets.
    void start();

    // Signals the thread to exit and joins.
    void stop();

private:
    void run(); // main poll loop (see ADR-020)

    bool running_ = false;
    std::thread thread_;

    zmq::context_t &zmq_ctx_;
    zmq::socket_t agentos_sock_;   // ZMQ_ROUTER bound to agentos.sock
    zmq::socket_t inproc_pull_;    // ZMQ_PULL  bound to inproc://gateway-out

    Central &central_;
};

} // namespace agentos
