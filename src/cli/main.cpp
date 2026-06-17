/**
 * agentos/src/cli/main.cpp
 *
 * AgentOS — entry point.
 *
 * `agentos run` constructs Central and blocks until SIGTERM/SIGINT.
 * All other subcommands are implemented via the CLI11 framework and
 * registered by their respective register_* functions.
 */

#include <CLI/CLI.hpp>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "agentos/central.h"
#include "agentos/config.h"
#include "agentos/home_init.h"

#include "agentos/adviser_params.h"
#include "agentos/cli_color.h"
#include "agentos/cli_completion.h"
#include "agentos/forge_params.h"
#include "agentos/job_params.h"
#include "agentos/review_params.h"
#include "agentos/worker_params.h"

// Forward declarations of subcommand registration functions.
void register_key_commands (CLI::App &app);
void register_job_commands (CLI::App &app);
void register_worker_commands (CLI::App &app);
void register_adviser_commands (CLI::App &app);
void register_review_commands (CLI::App &app);
void register_forge_commands (CLI::App &app);
void register_user_commands (CLI::App &app);

namespace
{

  void print_banner ()
  {
    std::cout << R"(
    ___                    __  ____  _____
   /   | ____ ____  ____  / /_/ __ \/ ___/
  / /| |/ __ `/ _ \/ __ \/ __/ / / /\__ \
 / ___ / /_/ /  __/ / / / /_/ /_/ /___/ /
/_/  |_\__, /\___/_/ /_/\__/\____//____/
       /____/   v)"
              << "0.0.1" << "\n"
              << "  Core:    Single-binary Agent Runtime  |  C++23\n";
  }

} // anonymous namespace

// plain function for signal(2)
static agentos::Central *g_central = nullptr;

static void signal_handler (int)
{
  if (g_central)
    g_central->stop ();
}

int main (int argc, char **argv)
{
  // Detect --complete early: completion mode must be fully silent.
  bool completing = false;
  for (int i = 1; i < argc; ++i)
    {
      if (std::string_view (argv[i]) == "--complete")
        {
          completing = true;
          break;
        }
    }

  // Logger exists for the whole process but stays silent outside `run`.
  auto logger = spdlog::stdout_color_mt ("agentos");
  logger->set_pattern ("[%H:%M:%S] [%^%l%$] %v");
  spdlog::set_default_logger (logger);
  spdlog::set_level (spdlog::level::off);

  if (!completing)
    print_banner ();

  CLI::App app{"AgentOS — AI agent orchestration daemon"};
  app.require_subcommand (1);

  bool no_color_flag = false;
  app.add_option ("--timeout", "Socket timeout in milliseconds")
    ->default_val (5000);
  app.add_option ("--socket", "Path to daemon socket");
  app.add_flag ("--json", "JSON output mode");
  app.add_flag ("--no-color", no_color_flag, "Disable terminal color");

  // ---------- daemon mode ----------
  auto *run = app.add_subcommand ("run", "Start the AgentOS daemon");
  run->callback (
    [&]
    {
      print_banner ();
      spdlog::set_level (spdlog::level::info);
      spdlog::info ("AgentOS {} starting", "0.1.0");

      std::string error;
      auto config = agentos::load_config (
        (agentos::agentos_home () / "config.toml").string (), error);
      if (!config)
      {
        agentos::cli::die (2, std::string ("config: ") + error);
      }
      agentos::read_env_api_key (*config);

      agentos::Central central (*config);
      g_central = &central;
      std::signal (SIGINT, signal_handler);
      std::signal (SIGTERM, signal_handler);

      spdlog::info ("daemon starting");
      central.run ();
    });

  // ---------- register all subcommand groups ----------
  register_key_commands (app);
  register_job_commands (app);
  register_worker_commands (app);
  register_adviser_commands (app);
  register_review_commands (app);
  register_forge_commands (app);
  register_user_commands (app);

  agentos::cli::add_completion (&app);

  try
  {
    app.parse (argc, argv);
  }
  catch (const CLI::ParseError &e)
  {
    return app.exit (e);
  }

  if (no_color_flag)
  {
    agentos::cli::set_color_enabled (false);
  }

  return 0;
}
