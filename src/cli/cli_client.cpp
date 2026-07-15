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

#include "agentos/cli_client.h"
#include "agentos/cli_color.h"
#include "agentos/database.h"
#include "agentos/home_init.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <thread>
#include <zmq.hpp>

namespace agentos::cli
{

  // ---- socket path
  // ------------------------------------------------------------

  static std::string resolve_socket_path ()
  {
    const char *agentos_home = std::getenv ("AGENTOS_HOME");
    std::filesystem::path base;
    if (agentos_home && agentos_home[0] != '\0')
    {
      base = agentos_home;
    }
    else
    {
      const char *home = std::getenv ("HOME");
      if (!home || home[0] == '\0')
      {
        die (5, "HOME not set");
      }
      base = std::filesystem::path (home) / ".agentos";
    }
    base /= "run";
    base /= "agentos.sock";
    return base.string ();
  }

  // ZMQ requires a transport scheme; unix-domain sockets use ipc://<path>.
  // Production always passes a bare filesystem path here (from
  // resolve_socket_path()), which never contains "://". Tests, however,
  // construct CliClient directly with a full ZMQ endpoint URI (e.g.
  // "tcp://127.0.0.1:PORT" from a bound ROUTER's last_endpoint) via the
  // (socket_path, access_key, timeout_ms) constructor. Unconditionally
  // prepending "ipc://" here used to turn that into the nonsensical
  // "ipc://tcp://127.0.0.1:PORT" — parsed as an ipc:// connection to a
  // Unix-domain socket file literally named "tcp://127.0.0.1:PORT", which
  // never exists and never connects to the test's real TCP-bound socket.
  // If the input already carries a transport scheme, pass it through
  // unchanged; only bare paths get "ipc://" prepended.
  static std::string zmq_endpoint (const std::string &path)
  {
    if (path.find ("://") != std::string::npos)
      return path;
    return "ipc://" + path;
  }

  // ---- access key loading
  // -----------------------------------------------------

  static std::string load_admin_key ()
  {
    agentos::Database db;
    if (!db.open ())
    {
      die (5, "cannot open agentos.db");
    }
    auto keys = db.load_active_access_keys ();
    for (const auto &k : keys)
    {
      if (k.role == "admin")
        return k.key;
    }
    if (!keys.empty ())
      return keys.front ().key;
    die (3, "no active access key found — run: agentos key generate");
    return {};
  }

  // ---- UUID helper
  // ------------------------------------------------------------

  static std::string make_uuid ()
  {
    std::ifstream ifs ("/proc/sys/kernel/random/uuid");
    if (!ifs)
      die (5, "cannot read UUID");
    std::string s;
    std::getline (ifs, s);
    while (!s.empty () && (s.back () == '\n' || s.back () == '\r'))
      s.pop_back ();
    return s;
  }

  // ---- construction
  // -----------------------------------------------------------
  //
  // IPC connect is asynchronous. ZMQ buffers send() until the handshake
  // completes, so we do not block here — we trust ZMQ to flush the request
  // once the link is up. The poll-for-response loop in send() has its own
  // timeout (default 5s) which is the user-visible "daemon not responding"
  // budget. A local IPC handshake is normally complete within microseconds;
  // any wait beyond that means the daemon really is not there.
  //

  CliClient::CliClient (int timeout_ms)
    : socket_path_ (resolve_socket_path ()), access_key_ (load_admin_key ()),
      timeout_ms_ (timeout_ms), ctx_ (1), sock_ (ctx_, zmq::socket_type::dealer)
  {
    sock_.connect (zmq_endpoint (socket_path_));
  }

  CliClient::CliClient (std::string socket_path, std::string access_key,
                        int timeout_ms)
    : socket_path_ (std::move (socket_path)),
      access_key_ (std::move (access_key)), timeout_ms_ (timeout_ms), ctx_ (1),
      sock_ (ctx_, zmq::socket_type::dealer)
  {
    sock_.connect (zmq_endpoint (socket_path_));
  }

  void CliClient::set_socket_path (std::string path)
  {
    if (path.empty ())
      return;
    socket_path_ = std::move (path);
    sock_.close ();
    sock_ = zmq::socket_t (ctx_, zmq::socket_type::dealer);
    sock_.connect (zmq_endpoint (socket_path_));
  }

  void CliClient::set_access_key (std::string key)
  {
    access_key_ = std::move (key);
  }

  // ---- send
  // -------------------------------------------------------------------

  rapidjson::Document CliClient::send (std::string_view method,
                                       rapidjson::Document params)
  {
    rapidjson::Document req (rapidjson::kObjectType);
    auto &alloc = req.GetAllocator ();
    req.AddMember ("jsonrpc", "2.0", alloc);
    req.AddMember (
      "id", rapidjson::Value (make_uuid ().c_str (), alloc).Move (), alloc);
    req.AddMember (
      "key", rapidjson::Value (access_key_.c_str (), alloc).Move (), alloc);
    req.AddMember ("method", rapidjson::Value (method.data (), alloc).Move (),
                   alloc);
    req.AddMember ("params", params, alloc);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    req.Accept (w);

    zmq::message_t msg (buf.GetSize ());
    std::memcpy (msg.data (), buf.GetString (), buf.GetSize ());
    if (!sock_.send (msg, zmq::send_flags::none))
    {
      throw CliError ("ZMQ send failed");
    }

    zmq::pollitem_t items[] = {{sock_, 0, ZMQ_POLLIN, 0}};
    auto deadline = std::chrono::steady_clock::now ()
                    + std::chrono::milliseconds (timeout_ms_);

    for (;;)
    {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
        deadline - std::chrono::steady_clock::now ());
      if (remaining.count () <= 0)
        throw CliError ("daemon not responding (timeout)");

      if (zmq::poll (items, 1, remaining) == 0)
        throw CliError ("daemon not responding (timeout)");

      zmq::message_t reply;
      auto res = sock_.recv (reply, zmq::recv_flags::none);
      if (!res)
        continue;

      std::string reply_str (static_cast<const char *> (reply.data ()),
                             reply.size ());
      rapidjson::Document resp;
      if (resp.Parse (reply_str.c_str ()).HasParseError ())
      {
        throw CliError ("invalid JSON reply from daemon");
      }
      if (resp.HasMember ("error"))
      {
        const auto &err = resp["error"];
        std::string emsg = err.HasMember ("message")
                             ? err["message"].GetString ()
                             : "json-rpc error";
        throw CliError (emsg);
      }
      if (!resp.HasMember ("result"))
      {
        throw CliError ("missing result in reply");
      }
      rapidjson::Document out;
      out.CopyFrom (resp["result"], out.GetAllocator ());
      return out;
    }
  }

} // namespace agentos::cli
