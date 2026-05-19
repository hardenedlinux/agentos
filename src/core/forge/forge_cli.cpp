#include "forge_cli.hpp"
#include <iostream>
#include <zmq.hpp>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <spdlog/spdlog.h>

namespace agentos {

int ForgeCli::execute(int argc, char* argv[]) {
    if (argc < 3) {
        print_help();
        return 1;
    }

    std::string command = argv[2];
    if (command == "list") {
        list_jobs();
    } else if (command == "show") {
        if (argc < 4) {
            std::cerr << "Usage: agentos review show <id>" << std::endl;
            return 1;
        }
        show_job(argv[3]);
    } else if (command == "approve") {
        if (argc < 4) {
            std::cerr << "Usage: agentos review approve <id>" << std::endl;
            return 1;
        }
        approve_job(argv[3]);
    } else if (command == "reject") {
        if (argc < 4) {
            std::cerr << "Usage: agentos review reject <id> -m <reason>" << std::endl;
            return 1;
        }
        std::string id = argv[3];
        std::string reason;
        for (int i = 4; i < argc - 1; ++i) {
            if (std::string(argv[i]) == "-m" && i + 1 < argc) {
                reason = argv[i + 1];
                break;
            }
        }
        reject_job(id, reason);
    } else {
        print_help();
        return 1;
    }
    return 0;
}

void ForgeCli::print_help() {
    std::cout << "Usage: agentos review <command> [args]" << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  list                    List all forge jobs" << std::endl;
    std::cout << "  show <id>               Show details of a forge job" << std::endl;
    std::cout << "  approve <id>            Approve a forge job in human review" << std::endl;
    std::cout << "  reject <id> -m <reason> Reject a forge job with reason" << std::endl;
}

void ForgeCli::list_jobs() {
    // Connect to daemon via ZMQ
    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, zmq::socket_type::req);
    sock.connect("ipc:///tmp/agentos_forge.sock");

    // Send request
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("method"); w.String("forge.list");
    w.Key("params"); w.String("{}");
    w.EndObject();

    zmq::message_t request(buf.GetString(), buf.GetSize());
    sock.send(request, zmq::send_flags::none);

    // Receive response
    zmq::message_t reply;
    sock.recv(reply, zmq::recv_flags::none);
    std::string response(static_cast<char*>(reply.data()), reply.size());

    // Parse and display
    rapidjson::Document doc;
    doc.Parse(response.c_str());
    if (doc.IsArray()) {
        for (const auto& job : doc.GetArray()) {
            std::cout << "ID: " << job["id"].GetString()
                      << " Method: " << job["method"].GetString()
                      << " Phase: " << job["phase"].GetString()
                      << " Attempt: " << job["attempt"].GetInt()
                      << "/" << job["max_attempts"].GetInt()
                      << std::endl;
        }
    }
}

void ForgeCli::show_job(const std::string& id) {
    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, zmq::socket_type::req);
    sock.connect("ipc:///tmp/agentos_forge.sock");

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("method"); w.String("forge.show");
    w.Key("params");
    w.StartObject();
    w.Key("id"); w.String(id.c_str());
    w.EndObject();
    w.EndObject();

    zmq::message_t request(buf.GetString(), buf.GetSize());
    sock.send(request, zmq::send_flags::none);

    zmq::message_t reply;
    sock.recv(reply, zmq::recv_flags::none);
    std::string response(static_cast<char*>(reply.data()), reply.size());

    rapidjson::Document doc;
    doc.Parse(response.c_str());
    if (doc.HasMember("error")) {
        std::cerr << "Error: " << doc["error"].GetString() << std::endl;
        return;
    }
    std::cout << "ID: " << doc["id"].GetString() << std::endl;
    std::cout << "Method: " << doc["method"].GetString() << std::endl;
    std::cout << "Requirement: " << doc["requirement"].GetString() << std::endl;
    std::cout << "Phase: " << doc["phase"].GetString() << std::endl;
    std::cout << "Attempt: " << doc["attempt"].GetInt() << "/" << doc["max_attempts"].GetInt() << std::endl;
    std::cout << "Last Code: " << doc["last_code"].GetString() << std::endl;
    std::cout << "Last Feedback: " << doc["last_feedback"].GetString() << std::endl;
}

void ForgeCli::approve_job(const std::string& id) {
    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, zmq::socket_type::req);
    sock.connect("ipc:///tmp/agentos_forge.sock");

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("method"); w.String("forge.approve");
    w.Key("params");
    w.StartObject();
    w.Key("id"); w.String(id.c_str());
    w.EndObject();
    w.EndObject();

    zmq::message_t request(buf.GetString(), buf.GetSize());
    sock.send(request, zmq::send_flags::none);

    zmq::message_t reply;
    sock.recv(reply, zmq::recv_flags::none);
    std::string response(static_cast<char*>(reply.data()), reply.size());
    std::cout << response << std::endl;
}

void ForgeCli::reject_job(const std::string& id, const std::string& reason) {
    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, zmq::socket_type::req);
    sock.connect("ipc:///tmp/agentos_forge.sock");

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("method"); w.String("forge.reject");
    w.Key("params");
    w.StartObject();
    w.Key("id"); w.String(id.c_str());
    w.Key("reason"); w.String(reason.c_str());
    w.EndObject();
    w.EndObject();

    zmq::message_t request(buf.GetString(), buf.GetSize());
    sock.send(request, zmq::send_flags::none);

    zmq::message_t reply;
    sock.recv(reply, zmq::recv_flags::none);
    std::string response(static_cast<char*>(reply.data()), reply.size());
    std::cout << response << std::endl;
}

} // namespace agentos
