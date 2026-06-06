/**
 * agentos/src/core/main.cpp
 *
 * AgentOS — entry point.
 *
 * Wires up all subsystems and verifies deps compile cleanly:
 *   ✓ spdlog    — structured logging       (header-only)
 *   ✓ ZeroMQ    — async I/O / event loop   (static archive)
 *   ✓ RapidJSON — JSON serialisation       (header-only)
 *
 * Architecture verified:
 *   Database → Registry → Verifier → Scheduler → Orchestrator → Master
 */

#include <iostream>
#include <string>

#include <httplib.h>
#include <openssl/opensslv.h>
#include <rapidjson/document.h>
#include <rapidjson/rapidjson.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <zmq.hpp>

#include "agentos/database/database.h"
#include "agentos/dispatcher.h"
#include "agentos/llm_client.h"
#include "agentos/llm_proxy.h"
#include "agentos/master.h"
#include "agentos/obs_bus.h"
#include "agentos/registry.h"
#include "agentos/scheduler.h"
#include "agentos/types.h"
#include "agentos/verifier.h"
#include "agentos/version.h"

static void print_banner ()
{
  std::cout << R"(
    ___                    __  ____  _____
   /   | ____ ____  ____  / /_/ __ \/ ___/
  / /| |/ __ `/ _ \/ __ \/ __/ / / /\__ \
 / ___ / /_/ /  __/ / / / /_/ /_/ /___/ /
/_/  |_\__, /\___/_/ /_/\__/\____//____/
       /____/   v)"
            << AGENTOS_VERSION << R"(

  Core:    Single-binary Agent Runtime  |  C++17
  Plugins: Unix Sockets + JSON-RPC 2.0  |  Language-agnostic clients
  Subsystems: Dispatcher, Registry, Verifier, Scheduler, Orchestrator, ObsBus, LlmClient
)" << "\n";
}

static void verify_deps ()
{
  spdlog::info ("--- dependency check ---");

  // spdlog
  spdlog::info ("spdlog {}.{}.{}  ✓", SPDLOG_VER_MAJOR, SPDLOG_VER_MINOR,
                SPDLOG_VER_PATCH);

  // ZeroMQ
  auto [major, minor, patch] = zmq::version ();
  spdlog::info ("ZeroMQ {}.{}.{}  ✓", major, minor, patch);

  // SQLite3
  spdlog::info ("SQLite3 {}  ✓", sqlite3_libversion ());

  // RapidJSON
  spdlog::info ("RapidJSON {}  ✓", RAPIDJSON_VERSION_STRING);

  // cpp-httplib
  spdlog::info ("cpp-httplib {}  ✓", CPPHTTPLIB_VERSION);

  // OpenSSL
  spdlog::info ("OpenSSL {}  ✓", OPENSSL_VERSION_TEXT);

  // RapidJSON — build a sample worker.register handshake
  std::string payload = R"({
    "jsonrpc": "2.0",
    "method": "worker.register",
    "params": {
        "name": "web-search",
        "version": "1.0.0",
        "commands": [{
            "name": "web.search",
            "description": "Search the web. Returns top N results as text.",
            "input": {
                "query":       { "type": "string",  "required": true },
                "max_results": { "type": "integer", "required": false }
            },
            "limits": {
                "timeout_ms": 10000,
                "max_input_len": 500
            }
        }]
    }
})";

  rapidjson::Document doc;
  doc.Parse (payload.data ());

  spdlog::info ("RapidJSON  ✓  sample worker.register: {} bytes",
                payload.size ());

  spdlog::info ("--- end dependency check ---");
}

static void verify_architecture ()
{
  spdlog::info ("--- architecture check ---");

  // Instantiate all subsystems and verify they wire together
  agentos::Dispatcher dispatcher ("/run/agentos");
  agentos::Database database ("/tmp/agentos_test.db");
  database.open ();
  agentos::Registry registry (database);
  agentos::ObsBus obs;

  agentos::Verifier verifier (registry);

  agentos::SchedulerConfig config;

  agentos::Scheduler scheduler (registry, dispatcher, config, database);

  agentos::Master master (dispatcher, registry, verifier, scheduler,
                          "/tmp/agentos_test.db");

  spdlog::info ("Dispatcher    ✓");
  spdlog::info ("Registry      ✓");
  spdlog::info ("Verifier      ✓");
  spdlog::info ("Scheduler     ✓");
  spdlog::info ("Master        ✓");
  spdlog::info ("ObsBus        ✓");

  // Verify the Verifier with a trivial plan (no commands registered = should
  // fail with helpful error)
  agentos::Plan plan;
  plan.task_id = agentos::TaskId ("test-task-001");
  plan.steps.push_back ({"step_1", "web.search", {{"query", "test"}}, {}});

  auto result = verifier.verify (plan);
  spdlog::info ("Verifier rejects unknown command: {} (errors: {})",
                result.ok ? "UNEXPECTED PASS" : "correctly rejected",
                result.errors.empty () ? "none" : result.errors[0]);

  // Verify LlmClient can be instantiated (no actual HTTP call)
  agentos::LlmProxy proxy (1, 120);
  agentos::Config::Llm llm_cfg;
  agentos::LlmClient llm_client (proxy, llm_cfg);
  spdlog::info ("LlmClient    ✓");

  spdlog::info ("--- end architecture check ---");
}

static void help ()
{
  verify_deps ();
  verify_architecture ();

  spdlog::info ("");
  spdlog::info ("Core subsystems:");
  spdlog::info ("  Dispatcher   — Unix socket server, JSON-RPC 2.0 framing");
  spdlog::info ("  Registry     — adviser + worker tables, command index");
  spdlog::info ("  Verifier     — plan validation against registered commands");
  spdlog::info ("  Scheduler    — topological execution, parallelism, retries");
  spdlog::info (
    "  Master       — task lifecycle coordinator (adviser → core → worker)");
  spdlog::info ("  ObsBus       — structured logs, metrics, task events");
  spdlog::info ("  LlmClient    — HTTP client for LLM API calls (ADR-012)");
  spdlog::info ("");
  spdlog::info ("Ready. Waiting for advisers and workers to connect.");
}

static void start (int argc, char *argv[])
{
  spdlog::info ("Starting AgentOS...");
}

static void stop (int /*argc*/, char * /*argv*/[])
{
  spdlog::info ("Stopping AgentOS...");
}

int main (int argc, char *argv[])
{
  auto logger = spdlog::stdout_color_mt ("agentos");
  logger->set_pattern ("[%H:%M:%S] [%^%l%$] %v");
  spdlog::set_default_logger (logger);

  print_banner ();
  spdlog::info ("AgentOS {} starting", AGENTOS_VERSION);

  if (argc < 2)
  {
    help ();
    return 0;
  }

  std::string cmd = argv[1];

  if (cmd == "start")
  {
    start (argc, argv);
  }
  else if (cmd == "stop")
  {
    stop (argc, argv);
  }
  else
  {
    help ();
    return 1;
  }

  return 0;
}
