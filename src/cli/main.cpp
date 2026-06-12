/**
 * agentos/src/cli/main.cpp
 *
 * AgentOS — entry point.
 *
 * `agentos run` constructs Central (ADR-024) and blocks until
 * SIGTERM/SIGINT. Other CLI subcommands (ADR-026) are not yet
 * implemented.
 */

#include <csignal>
#include <iostream>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "agentos/central.h"
#include "agentos/config.h"
#include "agentos/home_init.h"
#include "agentos/version.h"

namespace
{
  agentos::Central *g_central = nullptr;

  void signal_handler (int)
  {
    if (g_central)
      g_central->stop ();
  }

  void print_banner ()
  {
    std::cout
      << R"(
    ___                    __  ____  _____
   /   | ____ ____  ____  / /_/ __ \/ ___/
  / /| |/ __ `/ _ \/ __ \/ __/ / / /\__ \
 / ___ / /_/ /  __/ / / / /_/ /_/ /___/ /
/_/  |_\__, /\___/_/ /_/\__/\____//____/
       /____/   v)"
      << AGENTOS_VERSION << R"(

  Core:    Single-binary Agent Runtime  |  C++)"
      << __cplusplus << "\n";
  }

  int run_daemon ()
  {
    std::string error;
    auto config = agentos::load_config (
      (agentos::agentos_home () / "config.toml").string (), error);
    if (!config)
      {
        spdlog::critical ("failed to load config: {}", error);
        return 1;
      }
    agentos::read_env_api_key (*config);

    agentos::Central central (*config);
    g_central = &central;
    std::signal (SIGINT, signal_handler);
    std::signal (SIGTERM, signal_handler);

    central.run ();
    return 0;
  }
} // namespace

int main (int argc, char *argv[])
{
  auto logger = spdlog::stdout_color_mt ("agentos");
  logger->set_pattern ("[%H:%M:%S] [%^%l%$] %v");
  spdlog::set_default_logger (logger);

  print_banner ();
  spdlog::info ("AgentOS {} starting", AGENTOS_VERSION);

  if (argc < 2)
    {
      spdlog::info ("Usage: agentos run");
      spdlog::info ("(other CLI subcommands per ADR-026 not yet implemented)");
      return 0;
    }

  const std::string cmd = argv[1];

  if (cmd == "run")
    return run_daemon ();

  spdlog::info ("Usage: agentos run");
  spdlog::info ("(other CLI subcommands per ADR-026 not yet implemented)");
  return 1;
}
