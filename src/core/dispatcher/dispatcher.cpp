#include "agentos/dispatcher.h"
#include <spdlog/spdlog.h>

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
    // TODO Phase 0:
    //   zmq_ctx_new + zmq_socket (ZMQ_STREAM)
    //   zmq_bind(socket_path_)
    //   zmq_poll → on_new_connection callback
    //   Each connection: zmq_recv → frame parser (4-byte len prefix + JSON
    //   body) Dispatch to method_handlers_[method] on each complete message
    spdlog::info ("[dispatcher] listen() stub — socket: {}", socket_path_);
    return false;
  }

  void Dispatcher::send_request (
    const ClientId &client_id, const std::string &method,
    const std::string &params_json,
    std::function<void (const std::string &, const std::string &)> callback)
  {
    // TODO Phase 0:
    //   Generate UUID request id
    //   Store callback in pending_requests_[id]
    //   Write length-prefixed JSON-RPC request to client's pipe
    //   On inbound response matching id, invoke callback and remove from map
    spdlog::debug ("[dispatcher] send_request() stub — client:{} method:{}",
                   client_id, method);
    (void)params_json;
    (void)callback;
  }

  void Dispatcher::send_notification (const ClientId &client_id,
                                      const std::string &method,
                                      const std::string &params_json)
  {
    // TODO Phase 0: write notification (no id, no response expected)
    spdlog::debug (
                   "[dispatcher] send_notification() stub — client:{} method:{}", client_id,
                   method);
    (void)params_json;
  }

  void Dispatcher::stop ()
  {
    if (!running_)
      return;

    running_ = false;

    if (socket_)
    {
      zmq_close (socket_);
      socket_ = nullptr;
    }
    if (context_)
    {
      zmq_ctx_destroy (context_);
      context_ = nullptr;
    }

    spdlog::info ("[dispatcher] stop requested");
  }

} // namespace agentos
