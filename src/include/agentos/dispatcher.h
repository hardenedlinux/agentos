#pragma once
/**
 * agentos/dispatcher.h
 *
 * Dispatcher — owns the ZeroMQ socket server.
 *
 * Responsibilities (single):
 *   - Accept incoming connections from agents and executors
 *   - Frame / deframe length-prefixed JSON-RPC messages
 *   - Route inbound messages to registered method handlers
 *   - Send outbound requests and match responses by id
 *
 * The Dispatcher knows nothing about agents, executors, plans, or tasks.
 * It only speaks JSON-RPC 2.0 over a ZeroMQ socket.
 *
 * Message framing:
 *   [ 4 bytes: uint32 little-endian payload length ][ UTF-8 JSON payload ]
 */

#include "agentos/types.h"
#include "agentos/rpc.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <zmq.hpp>

namespace agentos
{

  // Called when a JSON-RPC request/notification arrives from a client.
  // Returns a non-empty string to send as the result; empty string = no
  // response (notification).
  using MethodHandler = std::function<std::string (
                                                   const ClientId &client_id, const std::string &method,
                                                   const std::string &params_json)>;

  // Called when a new client connects (before registration).
  using ConnectHandler = std::function<void (const ClientId &client_id)>;

  // Called when a client disconnects.
  using DisconnectHandler = std::function<void (const ClientId &client_id)>;

  class Dispatcher
  {
  public:
    explicit Dispatcher (const std::string &socket_path);
    ~Dispatcher ();

    // Register a handler for a JSON-RPC method name (e.g. "executor.register")
    void on_method (const std::string &method, MethodHandler handler);
    void on_connect (ConnectHandler handler);
    void on_disconnect (DisconnectHandler handler);

    // Start the event loop (blocks until stop() is called)
    bool listen ();

    // Send a JSON-RPC request to a specific client; invoke callback with
    // response
    void send_request (const ClientId &client_id, const std::string &method,
                       const std::string &params_json,
                       std::function<void (const std::string &result_json,
                                           const std::string &error_json)>
                         callback);

    // Send a JSON-RPC notification (no response expected)
    void send_notification (const ClientId &client_id,
                            const std::string &method,
                            const std::string &params_json);

    void stop ();

  private:
    std::string socket_path_;
    zmq::context_t *context_;
    zmq::socket_t *socket_;
    std::unordered_map<std::string, MethodHandler> method_handlers_;
    ConnectHandler connect_handler_;
    DisconnectHandler disconnect_handler_;
    bool running_;
  };

} // namespace agentos
