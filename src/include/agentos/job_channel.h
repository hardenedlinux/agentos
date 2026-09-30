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
#pragma once
/**
 * job_channel.h — ADR-042 Job execution channel.
 *
 * Each Worker run gets one end of a socketpair(AF_UNIX, SOCK_STREAM) as
 * fd 3 (AGENTOS_CHANNEL_FD). Frames are a 4-byte big-endian length
 * followed by that many bytes of JSON-RPC 2.0. The daemon end is owned by
 * ChannelServer: one thread polls every live run's socket, assembles
 * frames and hands each request, with the run's identity, to a callback
 * (the Orchestrator, which serves requests serially). Identity comes from
 * which socket a request arrived on — never from the request itself.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agentos
{
  // Defensive per-frame cap. Bulk data goes through files; the channel
  // carries ids and small JSON.
  inline constexpr std::size_t kChannelMaxFrame = 8u * 1024 * 1024;
  inline constexpr int kChannelFd = 3;

  // The run a channel belongs to, fixed at dispatch from the job record.
  struct ChannelContext
  {
    std::string run_id;
    std::string job_id;
    std::string step_id;
    std::string user_id;
    std::string worker_id;
    std::string job_dir;
  };

  class ChannelServer
  {
  public:
    using RequestCallback
      = std::function<void (const ChannelContext &, std::string frame)>;

    ChannelServer () = default;
    ~ChannelServer ();
    ChannelServer (const ChannelServer &) = delete;
    ChannelServer &operator= (const ChannelServer &) = delete;

    void set_request_callback (RequestCallback cb);

    // Create the socketpair for a run. Returns the child's end (to become
    // fd 3 in the Worker) or -1; the daemon's end is kept pending until
    // attach() once the fork succeeded, or discard() if it failed.
    int prepare (const ChannelContext &ctx);
    void attach (const std::string &run_id);
    void discard (const std::string &run_id);

    // Close the run's channel (Worker reaped). Pending replies are dropped.
    void close_run (const std::string &run_id);

    // Queue a framed reply to the run. Safe from any thread; dropped if the
    // run is gone.
    void send (const std::string &run_id, const std::string &message);

    // Encode one frame (length prefix + body). Exposed for tests/clients.
    static std::string frame (const std::string &body);

  private:
    struct Conn
    {
      ChannelContext ctx;
      int fd = -1;
      int child_fd = -1; // until attach()/discard()
      bool attached = false;
      std::string in;
      std::string out;
    };

    void ensure_thread ();
    void loop ();
    void wake ();
    void drop (Conn &c);

    std::mutex mu_;
    std::map<std::string, Conn> conns_; // by run_id
    RequestCallback cb_;
    std::thread thread_;
    int wake_fd_ = -1;
    bool stop_ = false;
  };
} // namespace agentos
