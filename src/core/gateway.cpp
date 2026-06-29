/**
 * agentos/gateway.cpp
 *
 * ADR-020 implementation — pure byte-pushing I/O thread for the external ZMQ
 * ROUTER.
 *
 * Frame layout:
 *   Inbound  (DEALER → ROUTER): [identity][payload]  (2 frames)
 *   Outbound (ROUTER → DEALER): [identity][payload]  (2 frames)
 *
 * DEALER sockets do NOT insert an empty delimiter frame.
 * The empty delimiter is a REQ/REP convention only.
 *
 * Outbound messages are enqueued via enqueue_outbound() from any thread and
 * flushed by the poll thread on every cycle — no inproc socket needed.
 */
#include "agentos/gateway.h"
#include "agentos/home_init.h"

#include <chrono>
#include <spdlog/spdlog.h>

namespace agentos
{

  Gateway::Gateway (zmq::context_t &zmq_ctx, ForwardFn forward_fn)
    : zmq_ctx_ (zmq_ctx),
      agentos_sock_ (zmq_ctx, zmq::socket_type::router),
      forward_fn_ (std::move (forward_fn))
  {
  }

  Gateway::~Gateway ()
  {
    stop ();
  }

  void Gateway::start ()
  {
    int mandatory = 1;
    agentos_sock_.set (zmq::sockopt::router_mandatory, mandatory);

    const std::string socket_path
      = (agentos_home () / "run" / "agentos.sock").string ();
    agentos_sock_.bind ("ipc://" + socket_path);
    spdlog::info ("[gateway] bound external socket at {}", socket_path);

    running_ = true;
    thread_  = std::thread (&Gateway::run, this);
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
    }
    catch (const zmq::error_t &)
    {
      // best-effort teardown
    }
    spdlog::info ("[gateway] stopped");
  }

  void Gateway::enqueue_outbound (std::string identity, std::string payload)
  {
    std::lock_guard lock (out_mutex_);
    out_queue_.push (OutboundMsg{ std::move (identity), std::move (payload) });
  }

  void Gateway::flush_outbound ()
  {
    std::queue<OutboundMsg> local;
    {
      std::lock_guard lock (out_mutex_);
      std::swap (local, out_queue_);
    }
    while (!local.empty ())
    {
      auto &msg = local.front ();
      try
      {
        if (!msg.identity.empty ())
        {
          zmq::message_t id_frame (msg.identity.data (), msg.identity.size ());
          auto r = agentos_sock_.send (id_frame, zmq::send_flags::sndmore);
          if (!r)
          {
            spdlog::warn ("[gateway] send identity frame failed, dropping message");
            local.pop ();
            continue;
          }
        }
        zmq::message_t payload_frame (msg.payload.data (), msg.payload.size ());
        auto r = agentos_sock_.send (payload_frame, zmq::send_flags::none);
        if (!r)
          spdlog::warn ("[gateway] send payload frame failed");
      }
      catch (const zmq::error_t &e)
      {
        // Client disconnected (EHOSTUNREACH) or other transient error.
        // Drop the message and continue — never crash the daemon for a
        // client that went away.
        spdlog::warn ("[gateway] send error ({}), dropping message for "
                      "identity '{}': {}",
                      e.num (),
                      msg.identity.empty () ? "(broadcast)" : msg.identity,
                      e.what ());
      }
      local.pop ();
    }
  }

  void Gateway::run ()
  {
    static constexpr int poll_timeout_ms = 100;

    spdlog::info ("[gateway] poll thread started");

    while (running_)
    {
      // ── 1. Flush outbound queue first ─────────────────────────────────
      flush_outbound ();

      // ── 2. Poll for inbound messages ──────────────────────────────────
      zmq::pollitem_t items[]
        = { { static_cast<void *> (agentos_sock_), 0, ZMQ_POLLIN, 0 } };

      try
        {
          zmq::poll (items, 1, std::chrono::milliseconds (poll_timeout_ms));
        }
      catch (const zmq::error_t &)
        {
          continue;
        }

      // ── 3. Flush again after poll — actors may have enqueued during wait
      flush_outbound ();

      // ── 4. Handle inbound ─────────────────────────────────────────────
      if (items[0].revents & ZMQ_POLLIN)
        {
          spdlog::info ("[gateway] inbound message arrived");

          zmq::message_t identity_msg;
          if (!agentos_sock_.recv (identity_msg, zmq::recv_flags::dontwait)
                 .has_value ())
            continue;

          std::string identity = identity_msg.to_string ();

          zmq::message_t payload_msg;
          if (!agentos_sock_.recv (payload_msg, zmq::recv_flags::dontwait)
                 .has_value ())
            continue;

          forward_fn_ (GatewayInbound{ .identity = std::move (identity),
                                       .message  = payload_msg.to_string () });
        }
    }
    spdlog::info ("[gateway] poll thread exited");
  }

} // namespace agentos
