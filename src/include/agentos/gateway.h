#pragma once
/**
 * agentos/gateway.h
 *
 * ADR-020: Gateway thread — pure I/O for the external ZMQ ROUTER socket.
 * Owns agentos.sock (ROUTER) and an outbound queue.
 *
 * Outbound path:
 *   Any actor calls Gateway::enqueue_outbound() — thread-safe enqueue.
 *   The Gateway poll thread drains the queue and sends frames directly on
 *   agentos_sock_, making it the sole owner of that socket.
 *
 * Inbound path:
 *   The poll thread receives [identity][payload] from DEALER clients and
 *   invokes forward_fn for each message.
 *
 * No inproc socket is used; the earlier PUSH/PULL inproc design was removed
 * because ZMQ PUSH/PULL does not reliably propagate the SNDMORE flag on the
 * inproc transport, causing multi-frame messages to be split across poll
 * cycles.
 */

#include "agentos/types.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <zmq.hpp>

namespace agentos
{

  class Gateway
  {
  public:
    using ForwardFn = std::function<void (GatewayInbound)>;

    Gateway (zmq::context_t &zmq_ctx, ForwardFn forward_fn);
    ~Gateway ();

    Gateway (const Gateway &)            = delete;
    Gateway &operator= (const Gateway &) = delete;

    // Bind the external ROUTER socket and start the I/O thread.
    void start ();

    // Signal the I/O thread to exit and join it.
    void stop ();

    // Enqueue an outbound message. Thread-safe; callable from any actor.
    void enqueue_outbound (std::string identity, std::string payload);

  private:
    void run ();

    // Drain the outbound queue and send on agentos_sock_.
    // Called only from the Gateway poll thread.
    void flush_outbound ();

    std::atomic<bool> running_{ false };
    std::thread       thread_;

    zmq::context_t &zmq_ctx_;
    zmq::socket_t   agentos_sock_; // ZMQ_ROUTER bound to agentos.sock
    ForwardFn       forward_fn_;

    // Outbound queue — written by any thread, drained by the poll thread.
    struct OutboundMsg
    {
      std::string identity;
      std::string payload;
    };
    std::mutex              out_mutex_;
    std::queue<OutboundMsg> out_queue_;
  };

} // namespace agentos
