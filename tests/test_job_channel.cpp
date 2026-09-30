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

// ADR-042 Job execution channel: transport (framing, limits, lifecycle)
// and the Python client Workers use.

#include "agentos/home_init.h"
#include "agentos/job_channel.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using agentos::ChannelContext;
using agentos::ChannelServer;

namespace
{
  std::string read_exact (int fd, std::size_t n)
  {
    std::string out;
    while (out.size () < n)
    {
      char buf[4096];
      const ssize_t r
        = ::read (fd, buf, std::min (sizeof buf, n - out.size ()));
      if (r <= 0)
        break;
      out.append (buf, static_cast<std::size_t> (r));
    }
    return out;
  }

  std::string read_frame (int fd)
  {
    const std::string hdr = read_exact (fd, 4);
    if (hdr.size () != 4)
      return "<eof>";
    const auto *p = reinterpret_cast<const unsigned char *> (hdr.data ());
    const std::size_t n = (std::size_t (p[0]) << 24) | (std::size_t (p[1]) << 16)
                          | (std::size_t (p[2]) << 8) | std::size_t (p[3]);
    return read_exact (fd, n);
  }

  struct Inbox
  {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<ChannelContext, std::string>> got;
    bool wait (std::size_t n, int ms = 2000)
    {
      std::unique_lock<std::mutex> lk (mu);
      return cv.wait_for (lk, std::chrono::milliseconds (ms),
                          [&] { return got.size () >= n; });
    }
  };
} // namespace

TEST (JobChannelTest, RequestCarriesRunIdentityAndReplyIsFramed)
{
  ChannelServer server;
  Inbox inbox;
  server.set_request_callback (
    [&] (const ChannelContext &ctx, std::string body)
    {
      std::lock_guard<std::mutex> lk (inbox.mu);
      inbox.got.emplace_back (ctx, std::move (body));
      inbox.cv.notify_all ();
    });

  const int child = server.prepare (
    ChannelContext{"run-1", "job-1", "step-0", "0", "w", "/tmp"});
  ASSERT_GE (child, 0);
  const int worker_end = ::dup (child);
  server.attach ("run-1");

  const std::string req = R"({"jsonrpc":"2.0","id":"1","method":"x"})";
  const std::string f = ChannelServer::frame (req);
  // Deliver in two pieces: the server must reassemble frames.
  ASSERT_EQ (::write (worker_end, f.data (), 3), 3);
  ::usleep (20000);
  ASSERT_EQ (::write (worker_end, f.data () + 3, f.size () - 3),
             static_cast<ssize_t> (f.size () - 3));

  ASSERT_TRUE (inbox.wait (1));
  EXPECT_EQ (inbox.got[0].second, req);
  EXPECT_EQ (inbox.got[0].first.run_id, "run-1");
  EXPECT_EQ (inbox.got[0].first.job_id, "job-1");
  EXPECT_EQ (inbox.got[0].first.user_id, "0");

  server.send ("run-1", R"({"jsonrpc":"2.0","id":"1","result":{}})");
  EXPECT_EQ (read_frame (worker_end), R"({"jsonrpc":"2.0","id":"1","result":{}})");

  server.close_run ("run-1");
  EXPECT_EQ (read_frame (worker_end), "<eof>") << "closed with the run";
  ::close (worker_end);
}

TEST (JobChannelTest, OversizedFrameClosesChannel)
{
  ChannelServer server;
  Inbox inbox;
  server.set_request_callback (
    [&] (const ChannelContext &ctx, std::string body)
    {
      std::lock_guard<std::mutex> lk (inbox.mu);
      inbox.got.emplace_back (ctx, std::move (body));
      inbox.cv.notify_all ();
    });
  const int child = server.prepare (ChannelContext{"r", "j", "s", "0", "w", "/"});
  const int worker_end = ::dup (child);
  server.attach ("r");

  const std::uint32_t n = agentos::kChannelMaxFrame + 1;
  const unsigned char hdr[4] = {static_cast<unsigned char> (n >> 24),
                                static_cast<unsigned char> (n >> 16),
                                static_cast<unsigned char> (n >> 8),
                                static_cast<unsigned char> (n)};
  ASSERT_EQ (::write (worker_end, hdr, 4), 4);
  const std::string err = read_frame (worker_end);
  EXPECT_NE (err.find ("8 MiB"), std::string::npos) << err;
  EXPECT_EQ (read_frame (worker_end), "<eof>");
  EXPECT_FALSE (inbox.wait (1, 200)) << "nothing delivered";
  ::close (worker_end);
}

TEST (JobChannelTest, PythonClientRoundTrip)
{
  char tmpl[] = "/tmp/agentos_chan_XXXXXX";
  const fs::path home = ::mkdtemp (tmpl);
  agentos::initialise_home (home);
  const fs::path sdk = home / "skills" / "agentos_channel.py";
  ASSERT_TRUE (fs::exists (sdk));

  ChannelServer server;
  server.set_request_callback (
    [&] (const ChannelContext &ctx, std::string body)
    {
      // Echo the request's params back as the result.
      const auto p = body.find ("\"params\":");
      const std::string params
        = p == std::string::npos ? "{}" : body.substr (p + 9, body.size () - p - 10);
      server.send (ctx.run_id,
                   R"({"jsonrpc":"2.0","id":"1","result":{"echo":)" + params
                     + R"(,"user":")" + ctx.user_id + R"("}})");
    });
  const int child = server.prepare (ChannelContext{"r", "j", "s", "u-7", "w", "/"});
  int out[2];
  ASSERT_EQ (::pipe (out), 0);
  const pid_t pid = ::fork ();
  if (pid == 0)
  {
    ::dup2 (child, 3);
    ::dup2 (out[1], 1);
    ::setenv ("AGENTOS_CHANNEL_FD", "3", 1);
    const std::string code
      = "import sys, json; sys.path.insert(0, '" + (home / "skills").string ()
        + "'); import agentos_channel as c; "
          "print(json.dumps(c.call('user.facts.get', {'a': 1}))); "
          "print(c.call('x', {'u': 'é'})['echo']['u'])";
    ::execlp ("python3", "python3", "-c", code.c_str (), nullptr);
    ::_exit (127);
  }
  ::close (out[1]);
  server.attach ("r");
  const std::string got = read_exact (out[0], 4096);
  int st = 0;
  ::waitpid (pid, &st, 0);
  ::close (out[0]);
  EXPECT_TRUE (WIFEXITED (st) && WEXITSTATUS (st) == 0) << got;
  EXPECT_NE (got.find (R"("echo": {"a": 1})"), std::string::npos) << got;
  EXPECT_NE (got.find (R"("user": "u-7")"), std::string::npos) << got;
  EXPECT_NE (got.find ("é"), std::string::npos) << "UTF-8 round trip: " << got;
  fs::remove_all (home);
}
