#pragma once

#include <rapidjson/document.h>
#include <zmq.hpp>

#include <string>
#include <string_view>
#include <stdexcept>

namespace agentos::cli {

class CliError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CliClient {
public:
    // Production constructor.
    explicit CliClient(int timeout_ms = 5000);

    // Test constructor: explicit socket path and access key.
    CliClient(std::string socket_path, std::string access_key,
              int timeout_ms = 5000);

    // Override the socket path (re‑connects).
    void set_socket_path(std::string path);

    // Override the access key.
    void set_access_key(std::string key);

    [[nodiscard]]
    rapidjson::Document send(std::string_view method,
                             rapidjson::Document params);

private:
    std::string    socket_path_;
    std::string    access_key_;
    int            timeout_ms_;
    zmq::context_t ctx_;
    zmq::socket_t  sock_;
};

} // namespace agentos::cli
