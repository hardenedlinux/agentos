#pragma once
/**
 * agentos/dispatcher.h
 *
 * Dispatcher — owns the ZeroMQ sockets per ADR-003.
 *
 * Socket layout:
 *   PULL  ipc:///var/run/agentos/results.sock   ← all agents push results here
 *   PUB   ipc:///var/run/agentos/events.sock    ← daemon broadcasts state events
 *   PUSH  ipc:///var/run/agentos/tasks/{task_id}.sock ← one per dispatched task,
 *                                                         created on demand,
 *                                                         torn down after agent exits
 *
 * The Dispatcher knows nothing about agents, workers, plans, or tasks.
 * It only manages ZMQ sockets and message framing.
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

  class Dispatcher
  {
  public:
    explicit Dispatcher (const std::string &socket_dir);
    ~Dispatcher ();

    // Bind the PULL and PUB sockets (called once at startup)
    bool bind ();

    // Start the event loop for the PULL socket (blocks until stop() is called)
    bool listen ();

    // Create a per-task PUSH socket, bind it, and return its path.
    // The caller should pass the path to the agent via environment variable.
    std::string create_task_push (const std::string &task_id);

    // Close and destroy a per-task PUSH socket (after agent exits)
    void close_task_push (const std::string &task_id);

    // Send a task JSON to a worker via the per-task PUSH socket.
    void send_task (const std::string &task_id, const std::string &task_json);

    // Receive a result JSON from the PULL socket (blocks).
    // Returns empty string on error or stop.
    std::string receive_result ();

    // Broadcast an event JSON via the PUB socket.
    void broadcast_event (const std::string &event_json);

    // Send a JSON-RPC request to a specific client (adviser) and invoke
    // callback with response. This is used for adviser communication.
    // For now, we use a simple synchronous approach.
    void send_request (const ClientId &client_id, const std::string &method,
                       const std::string &params_json,
                       std::function<void (const std::string &result_json,
                                           const std::string &error_json)>
                         callback);

    void stop ();

  private:
    std::string socket_dir_;
    zmq::context_t *context_;
    zmq::socket_t *pull_socket_;
    zmq::socket_t *pub_socket_;
    std::unordered_map<std::string, zmq::socket_t *> task_push_sockets_;
    bool running_;

    // Pending requests: request_id -> callback (for adviser communication)
    std::unordered_map<std::string,
                       std::function<void (const std::string &,
                                           const std::string &)>>
      pending_requests_;
  };

} // namespace agentos
