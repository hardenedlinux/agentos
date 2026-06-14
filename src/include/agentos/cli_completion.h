#pragma once

#include <CLI/App.hpp>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace agentos::cli {

// Call after all options and subcommands are registered on cmd.
// Adds a --complete flag hidden from --help that prints available
// subcommands and options to stdout, then exits 0.
// This is the sole mechanism powering bash tab completion (ADR-026).
inline void add_completion(CLI::App* cmd)
{
    cmd->add_flag("--complete", [cmd](std::int64_t) {
        for (auto* sub : cmd->get_subcommands(nullptr))
            std::cout << sub->get_name() << "\n";
        for (auto* opt : cmd->get_options(nullptr)) {
            const auto name = opt->get_name();
            if (name == "--complete" || name == "--help")
                continue;
            std::cout << name << "\n";
        }
        std::exit(0);
    })->group("");  // hidden from --help
}

} // namespace agentos::cli
