#include "agentos/dispatcher.h"
#include <spdlog/spdlog.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <cstring>
#include <thread>
#include <chrono>

namespace agentos
{

  Dispatcher::Dispatcher (const std::string &socket_path)
    : socket_path_ (socket_path),
      context_ (nullptr),
      socket_ (nullptr),
      running_ (false)
  {
  }

  Dispatcher::~Dispatcher ()
  {
    stop ();
  }

  void Dispatcher::on_method (const std::string &method, MethodHandler handler)
  {
    method_handlers_[method] = std::move (handler);
  }

  void Dispatcher::on_connect (ConnectHandler handler)
  {
    connect_handler_ = std::move (handler);
  }

  void Dispatcher::on_disconnect (DisconnectHandler handler)
  {
    disconnect_handler_ = std::move (handler);
  }

  bool Dispatcher::listen ()
  {
    spdlog::info ("[dispatcher] listen() — socket: {}", socket_path_);

    context_ = new zmq::context_t (1);
    socket_ = new zmq::socket_t (*context_, ZMQ_STREAM);
    socket_->set (zmq::sockopt::linger, 0);

    try
    {
      socket_->bind (socket_path_);
    }
    catch (const zmq::error_t &e)
    {
      spdlog::error ("[dispatcher] bind failed: {}", e.what ());
      return false;
    }

    running_ = true;

    // Buffer for incoming data
    std::unordered_map<zmq::message_t *, std::string> partial_buffers;

    while (running_)
    {
      zmq::message_t identity;
      zmq::message_t msg;

      try
      {
        // ZMQ_STREAM delivers identity frame then data frame
        if (!socket_->recv (identity, zmq::recv_flags::none))
          continue;
        if (!socket_->recv (msg, zmq::recv_flags::none))
          continue;
      }
      catch (const zmq::error_t &e)
      {
        if (running_)
          spdlog::error ("[dispatcher] recv error: {}", e.what ());
        break;
      }

      // Identity is the client's routing id (binary)
      std::string client_id (static_cast<const char *> (identity.data ()),
                             identity.size ());

      // Data frame may be partial; accumulate
      std::string &buf = partial_buffers[&identity];
      buf.append (static_cast<const char *> (msg.data ()), msg.size ());

      // Try to parse a complete message (length-prefixed)
      while (buf.size () >= 4)
      {
        uint32_t len;
        std::memcpy (&len, buf.data (), sizeof (len));
        if (buf.size () < 4 + len)
          break; // incomplete

        std::string payload = buf.substr (4, len);
        buf.erase (0, 4 + len);

        // Parse JSON-RPC
        rapidjson::Document doc;
        doc.Parse (payload.c_str ());
        if (doc.HasParseError ())
        {
          spdlog::warn ("[dispatcher] parse error from client {}", client_id);
          continue;
        }

        std::string method;
        if (doc.HasMember ("method") && doc["method"].IsString ())
          method = doc["method"].GetString ();

        std::string params_json;
        if (doc.HasMember ("params"))
        {
          rapidjson::StringBuffer sb;
          rapidjson::Writer<rapidjson::StringBuffer> w (sb);
          doc["params"].Accept (w);
          params_json = sb.GetString ();
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
          continue;
        }

        // It's a request or notification
        auto handler_it = method_handlers_.find (method);
        if (handler_it != method_handlers_.end ())
        {
          std::string response = handler_it->second (client_id, method, params_json);
          if (!response.empty ())
          {
            // Send response back
            std::string response_msg = response;
            uint32_t resp_len = static_cast<uint32_t> (response_msg.size ());
            std::string frame;
            frame.append (reinterpret_cast<const char *> (&resp_len), sizeof (resp_len));
            frame.append (response_msg);

            zmq::message_t resp_identity (identity.data (), identity.size ());
            zmq::message_t resp_frame (frame.data (), frame.size ());
            socket_->send (resp_identity, zmq::send_flags::sndmore);
            socket_->send (resp_frame, zmq::send_flags::none);
          }
        }
        else
        {
          spdlog::warn ("[dispatcher] unhandled method '{}' from client {}", method, client_id);
        }
      }

      // Clean up partial buffer when identity is no longer needed
      // (We keep it for the lifetime of the connection)
    }

    return true;
  }

  void Dispatcher::send_request (
    const ClientId &client_id, const std::string &method,
    const std::string &params_json,
    std::function<void (const std::string &, const std::string &)> callback)
  {
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

    // Store callback
    pending_requests_[request_id] = std::move (callback);

    // Send to client (identity is the client_id string)
    zmq::message_t identity (client_id.data (), client_id.size ());
    zmq::message_t msg (frame.data (), frame.size ());
    socket_->send (identity, zmq::send_flags::sndmore);
    socket_->send (msg, zmq::send_flags::none);

    spdlog::debug ("[dispatcher] send_request id={} client={} method={}", request_id, client_id, method);
  }

  void Dispatcher::send_notification (const ClientId &client_id,
                                      const std::string &method,
                                      const std::string &params_json)
  {
    // Build JSON-RPC notification (no id)
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w (sb);
    w.StartObject ();
    w.Key ("jsonrpc"); w.String ("2.0");
    w.Key ("method");  w.String (method.c_str ());
    w.Key ("params");
    w.RawValue (params_json.c_str (), params_json.size (), rapidjson::kObjectType);
    w.EndObject ();

    std::string payload = sb.GetString ();
    uint32_t len = static_cast<uint32_t> (payload.size ());
    std::string frame;
    frame.append (reinterpret_cast<const char *> (&len), sizeof (len));
    frame.append (payload);

    zmq::message_t identity (client_id.data (), client_id.size ());
    zmq::message_t msg (frame.data (), frame.size ());
    socket_->send (identity, zmq::send_flags::sndmore);
    socket_->send (msg, zmq::send_flags::none);

    spdlog::debug ("[dispatcher] send_notification client={} method={}", client_id, method);
  }

  void Dispatcher::stop ()
  {
    if (!running_)
      return;

    running_ = false;

    if (socket_)
    {
      socket_->close ();
      delete socket_;
      socket_ = nullptr;
    }
    if (context_)
    {
      delete context_;
      context_ = nullptr;
    }

    spdlog::info ("[dispatcher] stop requested");
  }

} // namespace agentos
