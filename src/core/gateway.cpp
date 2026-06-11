/**
 * agentos/gateway.cpp
 *
 * ADR-020 implementation — pure byte-pushing I/O thread for the external ZMQ
 * ROUTER.
 *
 * Frame layout:
 *   Inbound  (DEALER → ROUTER): ROUTER sees [identity][payload]  (2 frames)
 *   Outbound (inproc → ROUTER → DEALER): push [identity][payload], DEALER gets
 * [payload]
 *
 * DEALER sockets do NOT insert an empty delimiter frame.
 * The empty delimiter is a REQ/REP convention only.
 */
#include "agentos/gateway.h"
#include "agentos/home_init.h"

#include <chrono>
#include <spdlog/spdlog.h>

namespace agentos
{

  Gateway::Gateway (zmq::context_t &zmq_ctx, ForwardFn forward_fn)
    : zmq_ctx_ (zmq_ctx), agentos_sock_ (zmq_ctx, zmq::socket_type::router),
      inproc_pull_ (zmq_ctx, zmq::socket_type::pull),
      forward_fn_ (std::move (forward_fn))
  {
  }

  Gateway::~Gateway ()
  {
    stop ();
  }

  void Gateway::bind_inproc ()
  {
    inproc_pull_.bind ("inproc://gateway-out");
  }

  void Gateway::start ()
  {
    const std::string socket_path
      = (agentos_home () / "run" / "agentos.sock").string ();
    agentos_sock_.bind ("ipc://" + socket_path);
    spdlog::info ("[gateway] bound external socket at {}", socket_path);

    running_ = true;
    thread_ = std::thread (&Gateway::run, this);
  }

  void Gateway::stop ()
  {
    if (!running_)
      return;
    running_ = false;
    if (thread_.joinable ())
      thread_.join ();

    try
    {
      agentos_sock_.close ();
      inproc_pull_.close ();
    }
    catch (const zmq::error_t &)
    {
      // best-effort teardown
    }
    spdlog::info ("[gateway] stopped");
  }

  void Gateway::run ()
  {
    static constexpr int poll_timeout_ms = 100;

    while (running_)
    {
      zmq::pollitem_t items[]
        = {{static_cast<void *> (agentos_sock_), 0, ZMQ_POLLIN, 0},
           {static_cast<void *> (inproc_pull_), 0, ZMQ_POLLIN, 0}};

      try
      {
        zmq::poll (items, 2, std::chrono::milliseconds (poll_timeout_ms));
      }
      catch (const zmq::error_t &)
      {
        continue;
      }

      // ── 1. Drain all outbound messages first (inproc → external) ──────
      // Sender pushes [identity][payload]; forward both frames verbatim.
      // ROUTER uses the identity frame to route; DEALER receives [payload].
      if (items[1].revents & ZMQ_POLLIN)
      {
        while (running_)
        {
          zmq::message_t part;
          if (!inproc_pull_.recv (part, zmq::recv_flags::dontwait).has_value ())
            break;

          bool more = part.more ();
          agentos_sock_.send (std::move (part), more ? zmq::send_flags::sndmore
                                                     : zmq::send_flags::none);
          while (more)
          {
            zmq::message_t next;
            if (!inproc_pull_.recv (next, zmq::recv_flags::dontwait)
                   .has_value ())
              break;
            more = next.more ();
            agentos_sock_.send (std::move (next), more
                                                    ? zmq::send_flags::sndmore
                                                    : zmq::send_flags::none);
          }
        }
      }

      // ── 2. Handle at most one inbound message per cycle ──────────────
      // ROUTER receives [identity][payload] from a DEALER — exactly 2 frames.
      // No empty delimiter: DEALER does not use the REQ/REP envelope
      // convention.
      if (running_ && (items[0].revents & ZMQ_POLLIN))
      {
        zmq::message_t identity_msg;
        if (!agentos_sock_.recv (identity_msg, zmq::recv_flags::dontwait)
               .has_value ())
          continue;

        std::string identity = identity_msg.to_string ();

        // Second frame is the payload — no delimiter to skip.
        zmq::message_t payload_msg;
        if (!agentos_sock_.recv (payload_msg, zmq::recv_flags::dontwait)
            .has_value ())
          continue;

        forward_fn_ (GatewayInbound{.identity = std::move (identity),
                                    .message = payload_msg.to_string ()});
      }
    }
  }

} // namespace agentos
