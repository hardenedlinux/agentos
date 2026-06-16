#pragma once

#include <string>

namespace agentos::cli {

/// Register the `agentos vault` subcommands.
int vault_status(int argc, char **argv);
int vault_rekey(int argc, char **argv);

} // namespace agentos::cli
