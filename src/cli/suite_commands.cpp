#include <CLI/CLI.hpp>
#include <iostream>

namespace agentos::cli
{

void register_suite_commands (CLI::App *parent)
{
  auto *suite
    = parent->add_subcommand ("suite", "Manage Capability Suites (ADR-030)");

  suite->add_subcommand ("list", "List installed suites")
    ->callback (
      [] ()
      { std::cout << "suite list (not yet implemented)\n"; });

  suite->add_subcommand ("show", "Show suite details")
    ->callback (
      [] ()
      { std::cout << "suite show (not yet implemented)\n"; });

  suite->add_subcommand ("install", "Install a suite")
    ->callback (
      [] ()
      { std::cout << "suite install (not yet implemented)\n"; });

  suite->add_subcommand ("remove", "Remove a suite")
    ->callback (
      [] ()
      { std::cout << "suite remove (not yet implemented)\n"; });

  suite->add_subcommand ("update", "Update a suite")
    ->callback (
      [] ()
      { std::cout << "suite update (not yet implemented)\n"; });
}

} // namespace agentos::cli
