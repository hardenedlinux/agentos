/**
 * Copyright (C) 2026  HardenedLinux community
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

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
#include <optional>
#include <spdlog/spdlog.h>

namespace agentos
{

  namespace
  {

    // Receives exactly one frame, bounded by `timeout`. Deliberately never
    // a truly blocking recv() call — ZMQ's multi-part delivery is supposed
    // to land every frame of one message atomically, but this doesn't
    // trust that assumption blindly: if it's ever violated (a malformed
    // client, a future bridge that doesn't speak DEALER framing
    // correctly, or a libzmq surprise), this still returns instead of
    // hanging the Gateway thread forever. Used for both the identity and
    // payload frames of an inbound message, each with its own independent
    // timeout — see the bug this replaces: the previous code used two
    // independent recv_flags::dontwait calls with no timeout/retry at
    // all, so if the payload frame wasn't immediately available the
    // identity frame already read was silently discarded, desynchronizing
    // frame boundaries for every message read afterward.
    std::optional<zmq::message_t>
    recv_frame_with_timeout (zmq::socket_t &sock,
                             std::chrono::milliseconds timeout)
    {
      auto deadline = std::chrono::steady_clock::now () + timeout;
      for (;;)
      {
        zmq::message_t msg;
        auto res = sock.recv (msg, zmq::recv_flags::dontwait);
        if (res.has_value ())
          return msg;

        auto remaining
          = std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - std::chrono::steady_clock::now ());
        if (remaining.count () <= 0)
          return std::nullopt;

        zmq::pollitem_t items[]
          = {{static_cast<void *> (sock), 0, ZMQ_POLLIN, 0}};
        try
        {
          zmq::poll (items, 1, remaining);
        }
        catch (const zmq::error_t &)
        {
          return std::nullopt;
        }
        // loop back and try the non-blocking recv again
      }
    }

  } // unnamed namespace

  Gateway::Gateway (zmq::context_t &zmq_ctx, ForwardFn forward_fn)
    : zmq_ctx_ (zmq_ctx), agentos_sock_ (zmq_ctx, zmq::socket_type::router),
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
    out_queue_.push (OutboundMsg{std::move (identity), std::move (payload)});
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
            spdlog::warn (
              "[gateway] send identity frame failed, dropping message");
            local.pop ();
            continue;
          }
        }
        zmq::message_t payload_frame (msg.payload.data (), msg.payload.size ());
        auto r = agentos_sock_.send (payload_frame, zmq::send_flags::dontwait);
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
        = {{static_cast<void *> (agentos_sock_), 0, ZMQ_POLLIN, 0}};

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

        auto identity_msg = recv_frame_with_timeout (
          agentos_sock_, std::chrono::milliseconds (poll_timeout_ms));
        if (!identity_msg)
          continue;

        if (!identity_msg->more ())
        {
          spdlog::warn ("[gateway] single-frame message with no "
                        "continuation — dropping malformed message");
          continue;
        }

        auto payload_msg = recv_frame_with_timeout (
          agentos_sock_, std::chrono::milliseconds (poll_timeout_ms));
        if (!payload_msg)
        {
          spdlog::warn ("[gateway] identity frame arrived but payload "
                        "frame never followed within {}ms — dropping "
                        "incomplete message",
                        poll_timeout_ms);
          continue;
        }

        std::string identity = identity_msg->to_string ();
        forward_fn_ (GatewayInbound{.identity = std::move (identity),
                                    .message = payload_msg->to_string ()});
      }
    }
    spdlog::info ("[gateway] poll thread exited");
  }

} // namespace agentos
