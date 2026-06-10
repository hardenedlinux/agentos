/**
 * agentos/gateway.cpp
 *
 * ADR-020 implementation — pure byte‑pushing I/O thread for the external ZMQ ROUTER.
 */
#include "agentos/gateway.h"
#include "agentos/home_init.h"   // agentos_home()
#include "agentos/orchestrator.h" // GatewayInbound forwarding target
#include "agentos/central.h"      // Central::send<Orchestrator>(...)

#include <spdlog/spdlog.h>
#include <chrono>
#include <memory>

namespace agentos {

Gateway::Gateway(zmq::context_t &zmq_ctx, Central &central)
    : zmq_ctx_(zmq_ctx)
    , agentos_sock_(zmq_ctx, zmq::socket_type::router)
    , inproc_pull_(zmq_ctx, zmq::socket_type::pull)
    , central_(central)
{
}

Gateway::~Gateway()
{
    stop();
}

void Gateway::start()
{
    // 1. Bind inproc PULL endpoint *before* any PUSH publisher connects.
    inproc_pull_.bind("inproc://gateway-out");

    // 2. Bind external ROUTER socket.
    const std::string socket_path = (agentos_home() / "run" / "agentos.sock").string();
    agentos_sock_.bind("ipc://" + socket_path);
    spdlog::info("[gateway] bound external socket at {}", socket_path);

    running_ = true;
    thread_ = std::thread(&Gateway::run, this);
}

void Gateway::stop()
{
    if (!running_)
        return;
    running_ = false;
    if (thread_.joinable())
        thread_.join();

    try {
        agentos_sock_.close();
        inproc_pull_.close();
    } catch (const zmq::error_t &) {
        // best‑effort teardown
    }
    spdlog::info("[gateway] stopped");
}

void Gateway::run()
{
    // Poll timeout chosen short enough to keep the `running_` flag responsive.
    static constexpr int poll_timeout_ms = 100;

    while (running_) {
        zmq::pollitem_t items[] = {
            { static_cast<void *>(agentos_sock_.handle()), 0, ZMQ_POLLIN, 0 },
            { static_cast<void *>(inproc_pull_.handle()),  0, ZMQ_POLLIN, 0 }
        };

        try {
            zmq::poll(items, 2, std::chrono::milliseconds(poll_timeout_ms));
        } catch (const zmq::error_t &) {
            continue;
        }

        // ── 1. Drain all outbound messages first (inproc → external) ──────
        while (running_) {
            // Receive the first part of the multipart message pushed onto inproc.
            // The sender (Orchestrator / PeriodicExecutor) is responsible for
            // sending the complete ZMQ ROUTER‑compatible multipart:
            //   [identity frame] [empty delimiter frame] [payload]
            zmq::message_t part;
            if (!inproc_pull_.recv(part, zmq::recv_flags::dontwait))
                break;

            bool more = part.more();
            agentos_sock_.send(std::move(part),
                               more ? zmq::send_flags::sndmore
                                    : zmq::send_flags::none);

            // Forward any remaining parts of the multipart.
            while (more) {
                zmq::message_t next;
                if (!inproc_pull_.recv(next, zmq::recv_flags::dontwait)) {
                    // Unexpected end of multipart; break to avoid blocking.
                    break;
                }
                more = next.more();
                agentos_sock_.send(std::move(next),
                                   more ? zmq::send_flags::sndmore
                                        : zmq::send_flags::none);
            }
        }

        // ── 2. Handle at most one inbound message per cycle ──────────────
        if (running_) {
            zmq::message_t identity_msg;
            if (agentos_sock_.recv(identity_msg, zmq::recv_flags::dontwait)) {
                // The first part from a ROUTER socket is always the identity.
                std::string identity = identity_msg.to_string();

                // Discard the empty delimiter frame that the ROUTER socket inserts.
                zmq::message_t delim_msg;
                bool has_delim = agentos_sock_.recv(delim_msg, zmq::recv_flags::dontwait);

                // The next part is the actual JSON‑RPC payload.
                zmq::message_t payload_msg;
                bool has_payload = has_delim &&
                                   agentos_sock_.recv(payload_msg, zmq::recv_flags::dontwait);

                if (has_payload) {
                    GatewayInbound inbound;
                    inbound.identity = identity;
                    inbound.message = payload_msg.to_string();

                    // Forward *exactly* the raw bytes to the Orchestrator.
                    // Authentication / rate‑limiting happen there (ADR‑022).
                    central_.send<Orchestrator>(inbound);
                }
            }
        }
    }
}

} // namespace agentos
