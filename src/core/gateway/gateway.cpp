/**
 * agentos/gateway.cpp
 *
 * ADR-020 implementation — pure byte-pushing I/O thread for the external ZMQ
 * ROUTER.
 */
#include "agentos/gateway.h"
#include "agentos/central.h"      // Central::send<Orchestrator>(...)
#include "agentos/home_init.h"    // agentos_home()
#include "agentos/orchestrator.h" // GatewayInbound forwarding target

#include <chrono>
#include <spdlog/spdlog.h>

namespace agentos
{

  Gateway::Gateway (zmq::context_t &zmq_ctx, Central &central)
    : zmq_ctx_ (zmq_ctx), agentos_sock_ (zmq_ctx, zmq::socket_type::router),
      inproc_pull_ (zmq_ctx, zmq::socket_type::pull), central_ (central)
  {
  }

  Gateway::~Gateway ()
  {
    stop ();
  }

  void Gateway::start ()
  {
    // inproc://gateway-out PULL is bound by Central before this call (ADR-024).
    // Central guarantees the bind happens before any actor that may PUSH to it
    // starts.

    // Bind external ROUTER socket.
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
    // Poll timeout chosen short enough to keep the `running_` flag responsive.
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
      if (running_ && (items[0].revents & ZMQ_POLLIN))
      {
        zmq::message_t identity_msg;
        if (agentos_sock_.recv (identity_msg, zmq::recv_flags::dontwait)
              .has_value ())
        {
          std::string identity = identity_msg.to_string ();

          // Discard the empty delimiter frame that ROUTER inserts.
          zmq::message_t delim_msg;
          const bool has_delim
            = agentos_sock_.recv (delim_msg, zmq::recv_flags::dontwait)
                .has_value ();

          // The next part is the actual JSON-RPC payload.
          zmq::message_t payload_msg;
          const bool has_payload
            = has_delim
            && agentos_sock_.recv (payload_msg, zmq::recv_flags::dontwait)
            .has_value ();

          if (has_payload)
            {
              GatewayInbound inbound;
              inbound.identity = identity;
              inbound.message = payload_msg.to_string ();

              // Forward raw bytes to Orchestrator — no parsing, no auth
              // (ADR-022).
              central_.send<Orchestrator> (inbound);
            }
        }
      }
    }
  }

} // namespace agentos
