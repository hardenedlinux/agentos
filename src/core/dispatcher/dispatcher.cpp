#include "agentos/dispatcher.h"
#include <spdlog/spdlog.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <filesystem>

namespace agentos
{

  Dispatcher::Dispatcher (const std::string &socket_dir)
    : socket_dir_ (socket_dir),
      context_ (nullptr),
      pull_socket_ (nullptr),
      pub_socket_ (nullptr),
      running_ (false)
  {
  }

  Dispatcher::~Dispatcher ()
  {
    stop ();
  }

  bool Dispatcher::bind ()
  {
    spdlog::info ("[dispatcher] bind() — socket dir: {}", socket_dir_);

    context_ = new zmq::context_t (1);

    // PULL socket for results
    pull_socket_ = new zmq::socket_t (*context_, ZMQ_PULL);
    pull_socket_->set (zmq::sockopt::linger, 0);
    std::string pull_path = socket_dir_ + "/results.sock";
    try
    {
      pull_socket_->bind ("ipc://" + pull_path);
    }
    catch (const zmq::error_t &e)
    {
      spdlog::error ("[dispatcher] bind PULL failed: {}", e.what ());
      return false;
    }

    // PUB socket for events
    pub_socket_ = new zmq::socket_t (*context_, ZMQ_PUB);
    pub_socket_->set (zmq::sockopt::linger, 0);
    std::string pub_path = socket_dir_ + "/events.sock";
    try
    {
      pub_socket_->bind ("ipc://" + pub_path);
    }
    catch (const zmq::error_t &e)
    {
      spdlog::error ("[dispatcher] bind PUB failed: {}", e.what ());
      return false;
    }

    spdlog::info ("[dispatcher] bound PULL at {} and PUB at {}", pull_path, pub_path);
    return true;
  }

  bool Dispatcher::listen ()
  {
    if (!pull_socket_)
    {
      spdlog::error ("[dispatcher] listen() called before bind()");
      return false;
    }

    running_ = true;

    while (running_)
    {
      zmq::message_t msg;
      try
      {
        if (!pull_socket_->recv (msg, zmq::recv_flags::none))
          continue;
      }
      catch (const zmq::error_t &e)
      {
        if (running_)
          spdlog::error ("[dispatcher] recv error: {}", e.what ());
        break;
      }

      std::string payload (static_cast<const char *> (msg.data ()), msg.size ());

      // Parse JSON-RPC response (result from agent)
      rapidjson::Document doc;
      doc.Parse (payload.c_str ());
      if (doc.HasParseError ())
      {
        spdlog::warn ("[dispatcher] parse error on result");
        continue;
      }

      // Check for response (has "id" and "result" or "error")
      if (doc.HasMember ("id") && (doc.HasMember ("result") || doc.HasMember ("error")))
      {
        std::string id = doc["id"].GetString ();
        auto it = pending_requests_.find (id);
        if (it != pending_requests_.end ())
        {
          std::string result_json;
          std::string error_json;
          if (doc.HasMember ("result"))
          {
            rapidjson::StringBuffer sb;
            rapidjson::Writer<rapidjson::StringBuffer> w (sb);
            doc["result"].Accept (w);
            result_json = sb.GetString ();
          }
          if (doc.HasMember ("error"))
          {
            rapidjson::StringBuffer sb;
            rapidjson::Writer<rapidjson::StringBuffer> w (sb);
            doc["error"].Accept (w);
            error_json = sb.GetString ();
          }
          it->second (result_json, error_json);
          pending_requests_.erase (it);
        }
      }
      else
      {
        spdlog::warn ("[dispatcher] received message without id/result/error");
      }
    }

    return true;
  }

  std::string Dispatcher::create_task_push (const std::string &task_id)
  {
    std::string path = socket_dir_ + "/tasks/" + task_id + ".sock";
    // Ensure directory exists
    std::filesystem::create_directories (socket_dir_ + "/tasks");

    zmq::socket_t *push_sock = new zmq::socket_t (*context_, ZMQ_PUSH);
    push_sock->set (zmq::sockopt::linger, 0);
    try
    {
      push_sock->bind ("ipc://" + path);
    }
    catch (const zmq::error_t &e)
    {
      spdlog::error ("[dispatcher] bind PUSH for task {} failed: {}", task_id, e.what ());
      delete push_sock;
      return "";
    }

    task_push_sockets_[task_id] = push_sock;
    spdlog::info ("[dispatcher] created PUSH socket for task {} at {}", task_id, path);
    return path;
  }

  void Dispatcher::close_task_push (const std::string &task_id)
  {
    auto it = task_push_sockets_.find (task_id);
    if (it == task_push_sockets_.end ())
    {
      spdlog::warn ("[dispatcher] close_task_push: unknown task {}", task_id);
      return;
    }

    it->second->close ();
    delete it->second;
    task_push_sockets_.erase (it);
    spdlog::info ("[dispatcher] closed PUSH socket for task {}", task_id);
  }

  void Dispatcher::send_task (const std::string &task_id, const std::string &task_json)
  {
    auto it = task_push_sockets_.find (task_id);
    if (it == task_push_sockets_.end ())
    {
      spdlog::error ("[dispatcher] send_task: no PUSH socket for task {}", task_id);
      return;
    }

    // Frame: length-prefixed JSON
    uint32_t len = static_cast<uint32_t> (task_json.size ());
    std::string frame;
    frame.append (reinterpret_cast<const char *> (&len), sizeof (len));
    frame.append (task_json);

    zmq::message_t msg (frame.data (), frame.size ());
    it->second->send (msg, zmq::send_flags::none);
    spdlog::debug ("[dispatcher] sent task {} ({} bytes)", task_id, task_json.size ());
  }

  std::string Dispatcher::receive_result ()
  {
    if (!pull_socket_)
    {
      spdlog::error ("[dispatcher] receive_result() called before bind()");
      return "";
    }

    zmq::message_t msg;
    try
    {
      if (!pull_socket_->recv (msg, zmq::recv_flags::none))
        return "";
    }
    catch (const zmq::error_t &e)
    {
      spdlog::error ("[dispatcher] receive_result error: {}", e.what ());
      return "";
    }

    std::string payload (static_cast<const char *> (msg.data ()), msg.size ());
    // Expect length-prefixed JSON
    if (payload.size () < 4)
      return "";
    uint32_t len;
    std::memcpy (&len, payload.data (), sizeof (len));
    if (payload.size () < 4 + len)
      return "";
    return payload.substr (4, len);
  }

  void Dispatcher::broadcast_event (const std::string &event_json)
  {
    if (!pub_socket_)
    {
      spdlog::error ("[dispatcher] broadcast_event() called before bind()");
      return;
    }

    uint32_t len = static_cast<uint32_t> (event_json.size ());
    std::string frame;
    frame.append (reinterpret_cast<const char *> (&len), sizeof (len));
    frame.append (event_json);

    zmq::message_t msg (frame.data (), frame.size ());
    pub_socket_->send (msg, zmq::send_flags::none);
    spdlog::debug ("[dispatcher] broadcast event ({} bytes)", event_json.size ());
  }

  void Dispatcher::send_request (
    const ClientId &client_id, const std::string &method,
    const std::string &params_json,
    std::function<void (const std::string &, const std::string &)> callback)
  {
    // For adviser communication, we use a simple approach:
    // We create a temporary PUSH socket to the adviser's known address.
    // For now, we assume the adviser is listening on a PULL socket at
    // ipc:///var/run/agentos/advisers/{client_id}.sock
    std::string adviser_path = socket_dir_ + "/advisers/" + client_id.value() + ".sock";

    zmq::socket_t req_sock (*context_, ZMQ_PUSH);
    req_sock.set (zmq::sockopt::linger, 0);
    try
    {
      req_sock.connect ("ipc://" + adviser_path);
    }
    catch (const zmq::error_t &e)
    {
      spdlog::error ("[dispatcher] send_request connect to adviser {} failed: {}", client_id, e.what ());
      return;
    }

    std::string request_id = gen_new_uuid ();

    // Build JSON-RPC request
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w (sb);
    w.StartObject ();
    w.Key ("jsonrpc"); w.String ("2.0");
    w.Key ("id");      w.String (request_id.c_str ());
    w.Key ("method");  w.String (method.c_str ());
    w.Key ("params");
    w.RawValue (params_json.c_str (), params_json.size (), rapidjson::kObjectType);
    w.EndObject ();

    std::string payload = sb.GetString ();
    uint32_t len = static_cast<uint32_t> (payload.size ());
    std::string frame;
    frame.append (reinterpret_cast<const char *> (&len), sizeof (len));
    frame.append (payload);

    zmq::message_t msg (frame.data (), frame.size ());
    req_sock.send (msg, zmq::send_flags::none);

    // Store callback
    pending_requests_[request_id] = std::move (callback);

    spdlog::debug ("[dispatcher] send_request id={} client={} method={}", request_id, client_id, method);
  }

  void Dispatcher::stop ()
  {
    if (!running_)
      return;

    running_ = false;

    // Close all task push sockets
    for (auto &[task_id, sock] : task_push_sockets_)
    {
      sock->close ();
      delete sock;
    }
    task_push_sockets_.clear ();

    if (pull_socket_)
    {
      pull_socket_->close ();
      delete pull_socket_;
      pull_socket_ = nullptr;
    }
    if (pub_socket_)
    {
      pub_socket_->close ();
      delete pub_socket_;
      pub_socket_ = nullptr;
    }
    if (context_)
    {
      delete context_;
      context_ = nullptr;
    }

    spdlog::info ("[dispatcher] stop requested");
  }

} // namespace agentos
