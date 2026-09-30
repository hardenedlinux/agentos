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
#include "agentos/job_channel.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace agentos
{
  ChannelServer::~ChannelServer ()
  {
    {
      std::lock_guard<std::mutex> lk (mu_);
      stop_ = true;
    }
    wake ();
    if (thread_.joinable ())
      thread_.join ();
    for (auto &[id, c] : conns_)
    {
      if (c.fd >= 0)
        ::close (c.fd);
      if (c.child_fd >= 0)
        ::close (c.child_fd);
    }
    if (wake_fd_ >= 0)
      ::close (wake_fd_);
  }

  void ChannelServer::set_request_callback (RequestCallback cb)
  {
    std::lock_guard<std::mutex> lk (mu_);
    cb_ = std::move (cb);
  }

  std::string ChannelServer::frame (const std::string &body)
  {
    const auto n = static_cast<std::uint32_t> (body.size ());
    std::string out;
    out.reserve (4 + body.size ());
    out.push_back (static_cast<char> ((n >> 24) & 0xff));
    out.push_back (static_cast<char> ((n >> 16) & 0xff));
    out.push_back (static_cast<char> ((n >> 8) & 0xff));
    out.push_back (static_cast<char> (n & 0xff));
    out += body;
    return out;
  }

  void ChannelServer::ensure_thread ()
  {
    // mu_ held by caller
    if (thread_.joinable ())
      return;
    wake_fd_ = ::eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0)
    {
      spdlog::error ("[channel] eventfd failed: {}", std::strerror (errno));
      return;
    }
    thread_ = std::thread ([this] { loop (); });
  }

  void ChannelServer::wake ()
  {
    if (wake_fd_ >= 0)
    {
      const std::uint64_t one = 1;
      [[maybe_unused]] auto r = ::write (wake_fd_, &one, sizeof one);
    }
  }

  int ChannelServer::prepare (const ChannelContext &ctx)
  {
    int sv[2];
    if (::socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
    {
      spdlog::error ("[channel] socketpair failed for run {}: {}", ctx.run_id,
                     std::strerror (errno));
      return -1;
    }
    ::fcntl (sv[0], F_SETFL, ::fcntl (sv[0], F_GETFL) | O_NONBLOCK);
    std::lock_guard<std::mutex> lk (mu_);
    ensure_thread ();
    Conn c;
    c.ctx = ctx;
    c.fd = sv[0];
    c.child_fd = sv[1];
    conns_[ctx.run_id] = std::move (c);
    return sv[1];
  }

  void ChannelServer::attach (const std::string &run_id)
  {
    {
      std::lock_guard<std::mutex> lk (mu_);
      auto it = conns_.find (run_id);
      if (it == conns_.end ())
        return;
      if (it->second.child_fd >= 0)
      {
        ::close (it->second.child_fd); // the Worker has its own copy
        it->second.child_fd = -1;
      }
      it->second.attached = true;
    }
    wake ();
  }

  void ChannelServer::discard (const std::string &run_id)
  {
    close_run (run_id);
  }

  void ChannelServer::drop (Conn &c)
  {
    if (c.fd >= 0)
      ::close (c.fd);
    if (c.child_fd >= 0)
      ::close (c.child_fd);
    c.fd = c.child_fd = -1;
  }

  void ChannelServer::close_run (const std::string &run_id)
  {
    {
      std::lock_guard<std::mutex> lk (mu_);
      auto it = conns_.find (run_id);
      if (it == conns_.end ())
        return;
      drop (it->second);
      conns_.erase (it);
    }
    wake ();
  }

  void ChannelServer::send (const std::string &run_id,
                            const std::string &message)
  {
    {
      std::lock_guard<std::mutex> lk (mu_);
      auto it = conns_.find (run_id);
      if (it == conns_.end () || it->second.fd < 0)
      {
        spdlog::warn ("[channel] reply for closed run {} dropped", run_id);
        return;
      }
      it->second.out += frame (message);
    }
    wake ();
  }

  void ChannelServer::loop ()
  {
    std::vector<pollfd> pfds;
    std::vector<std::string> ids;
    for (;;)
    {
      pfds.clear ();
      ids.clear ();
      {
        std::lock_guard<std::mutex> lk (mu_);
        if (stop_)
          return;
        pfds.push_back ({wake_fd_, POLLIN, 0});
        ids.emplace_back ();
        for (auto &[id, c] : conns_)
          if (c.attached && c.fd >= 0)
          {
            short ev = POLLIN;
            if (!c.out.empty ())
              ev |= POLLOUT;
            pfds.push_back ({c.fd, ev, 0});
            ids.push_back (id);
          }
      }

      if (::poll (pfds.data (), pfds.size (), -1) < 0)
      {
        if (errno == EINTR)
          continue;
        spdlog::error ("[channel] poll failed: {}", std::strerror (errno));
        return;
      }
      if (pfds[0].revents & POLLIN)
      {
        std::uint64_t v;
        while (::read (wake_fd_, &v, sizeof v) > 0)
        {
        }
      }

      std::vector<std::pair<ChannelContext, std::string>> requests;
      RequestCallback cb;
      {
        std::lock_guard<std::mutex> lk (mu_);
        cb = cb_;
        for (std::size_t i = 1; i < pfds.size (); ++i)
        {
          auto it = conns_.find (ids[i]);
          if (it == conns_.end () || it->second.fd != pfds[i].fd)
            continue;
          Conn &c = it->second;

          if (pfds[i].revents & POLLOUT)
          {
            const ssize_t n = ::send (c.fd, c.out.data (), c.out.size (),
                                      MSG_NOSIGNAL);
            if (n > 0)
              c.out.erase (0, static_cast<std::size_t> (n));
            else if (n < 0 && errno != EAGAIN && errno != EINTR)
              c.out.clear ();
          }

          if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR))
          {
            char buf[65536];
            for (;;)
            {
              const ssize_t n = ::read (c.fd, buf, sizeof buf);
              if (n > 0)
              {
                c.in.append (buf, static_cast<std::size_t> (n));
                if (c.in.size () > kChannelMaxFrame + 4)
                  break;
                continue;
              }
              if (n == 0 || (errno != EAGAIN && errno != EINTR))
              {
                // Worker closed its end (or exited). Keep the entry until
                // close_run() so late replies are dropped quietly.
                ::close (c.fd);
                c.fd = -1;
              }
              break;
            }
            // Extract complete frames.
            while (c.fd >= 0 && c.in.size () >= 4)
            {
              const auto *p = reinterpret_cast<const unsigned char *> (
                c.in.data ());
              const std::size_t len = (std::size_t (p[0]) << 24)
                                      | (std::size_t (p[1]) << 16)
                                      | (std::size_t (p[2]) << 8)
                                      | std::size_t (p[3]);
              if (len > kChannelMaxFrame)
              {
                spdlog::error ("[channel] run {}: frame of {} bytes exceeds "
                               "the {} byte limit; closing channel",
                               c.ctx.run_id, len, kChannelMaxFrame);
                const std::string err
                  = frame (R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,)"
                           R"("message":"frame exceeds 8 MiB"}})");
                [[maybe_unused]] auto r = ::send (
                  c.fd, err.data (), err.size (), MSG_NOSIGNAL | MSG_DONTWAIT);
                ::close (c.fd);
                c.fd = -1;
                c.in.clear ();
                break;
              }
              if (c.in.size () < 4 + len)
                break;
              requests.emplace_back (c.ctx, c.in.substr (4, len));
              c.in.erase (0, 4 + len);
            }
          }
        }
      }
      if (cb)
        for (auto &[ctx, body] : requests)
          cb (ctx, std::move (body));
    }
  }
} // namespace agentos
