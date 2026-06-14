#include "agentos/cli_client.h"
#include "agentos/database.h"
#include "agentos/home_init.h"
#include "agentos/cli_color.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

namespace agentos::cli {

// ---- socket path ------------------------------------------------------------

static std::string resolve_socket_path()
{
    const char* agentos_home = std::getenv("AGENTOS_HOME");
    std::filesystem::path base;
    if (agentos_home && agentos_home[0] != '\0') {
        base = agentos_home;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || home[0] == '\0') {
            die(5, "HOME not set");
        }
        base = std::filesystem::path(home) / ".agentos";
    }
    base /= "run";
    base /= "agentos.sock";
    return base.string();
}

// ---- access key loading -----------------------------------------------------

static std::string load_admin_key()
{
    agentos::Database db;
    if (!db.open()) {
        die(5, "cannot open agentos.db");
    }
    auto keys = db.load_active_access_keys();
    for (const auto& k : keys) {
        if (k.role == "admin") return k.key;
    }
    if (!keys.empty()) return keys.front().key;
    die(3, "no active access key found — run: agentos key generate");
    return {};
}

// ---- UUID helper ------------------------------------------------------------

static std::string make_uuid()
{
    std::ifstream ifs("/proc/sys/kernel/random/uuid");
    if (!ifs) die(5, "cannot read UUID");
    std::string s;
    std::getline(ifs, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// ---- construction -----------------------------------------------------------

CliClient::CliClient(int timeout_ms)
    : socket_path_(resolve_socket_path()),
      access_key_(load_admin_key()),
      timeout_ms_(timeout_ms),
      ctx_(1),
      sock_(ctx_, zmq::socket_type::dealer)
{
    sock_.connect(socket_path_.c_str());
}

CliClient::CliClient(std::string socket_path, std::string access_key,
                     int timeout_ms)
    : socket_path_(std::move(socket_path)),
      access_key_(std::move(access_key)),
      timeout_ms_(timeout_ms),
      ctx_(1),
      sock_(ctx_, zmq::socket_type::dealer)
{
    sock_.connect(socket_path_.c_str());
}

void CliClient::set_socket_path(std::string path)
{
    if (path.empty()) return;
    socket_path_ = std::move(path);
    sock_.close();
    sock_ = zmq::socket_t(ctx_, zmq::socket_type::dealer);
    sock_.connect(socket_path_.c_str());
}

void CliClient::set_access_key(std::string key)
{
    access_key_ = std::move(key);
}

// ---- send -------------------------------------------------------------------

rapidjson::Document CliClient::send(std::string_view method,
                                     rapidjson::Document params)
{
    rapidjson::Document req(rapidjson::kObjectType);
    auto& alloc = req.GetAllocator();
    req.AddMember("jsonrpc", "2.0", alloc);
    req.AddMember("id", rapidjson::Value(make_uuid().c_str(), alloc).Move(), alloc);
    req.AddMember("key", rapidjson::Value(access_key_.c_str(), alloc).Move(), alloc);
    req.AddMember("method", rapidjson::Value(method.data(), alloc).Move(), alloc);
    req.AddMember("params", params, alloc);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    req.Accept(w);

    zmq::message_t msg(buf.GetSize());
    std::memcpy(msg.data(), buf.GetString(), buf.GetSize());
    if (!sock_.send(msg, zmq::send_flags::none)) {
        throw CliError("ZMQ send failed");
    }

    zmq::pollitem_t items[] = { { sock_, 0, ZMQ_POLLIN, 0 } };
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeout_ms_);

    for (;;) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
            throw CliError("daemon not responding (timeout)");

        if (zmq::poll(items, 1, remaining) == 0)
            throw CliError("daemon not responding (timeout)");

        zmq::message_t reply;
        auto res = sock_.recv(reply, zmq::recv_flags::none);
        if (!res) continue;

        std::string reply_str(static_cast<const char*>(reply.data()),
                               reply.size());
        rapidjson::Document resp;
        if (resp.Parse(reply_str.c_str()).HasParseError()) {
            throw CliError("invalid JSON reply from daemon");
        }
        if (resp.HasMember("error")) {
            const auto& err = resp["error"];
            std::string emsg = err.HasMember("message")
                                 ? err["message"].GetString()
                                 : "json-rpc error";
            throw CliError(emsg);
        }
        if (!resp.HasMember("result")) {
            throw CliError("missing result in reply");
        }
        rapidjson::Document out;
        out.CopyFrom(resp["result"], out.GetAllocator());
        return out;
    }
}

} // namespace agentos::cli
