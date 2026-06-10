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
            zmq::message_t msg;
            if (!inproc_pull_.recv(msg, zmq::recv_flags::dontwait))
                break;
            // Outbound messages pushed onto inproc://gateway-out are expected
            // to already contain the ZMQ identity frame as the first part.
            agentos_sock_.send(std::move(msg), zmq::send_flags::none);
        }

        // ── 2. Handle at most one inbound message per cycle ──────────────
        if (running_) {
            zmq::message_t msg;
            if (agentos_sock_.recv(msg, zmq::recv_flags::dontwait)) {
                // The first part from a ROUTER socket is always the identity.
                std::string identity = msg.to_string();

                // The next part is the JSON-RPC payload.
                zmq::message_t payload;
                bool have_payload = agentos_sock_.recv(payload, zmq::recv_flags::dontwait);
                if (have_payload) {
                    GatewayInbound inbound;
                    inbound.identity = identity;
                    inbound.message = payload.to_string();

                    // Forward *exactly* the raw bytes to the Orchestrator.
                    // Authentication / rate‑limiting happen there (ADR‑022).
                    central_.send<Orchestrator>(inbound);
                }
            }
        }
    }
}

} // namespace agentos
