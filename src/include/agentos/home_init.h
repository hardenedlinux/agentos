#ifndef AGENTOS_HOME_INIT_H
#define AGENTOS_HOME_INIT_H

#include <filesystem>

namespace agentos {

/// Returns the base directory for AgentOS runtime data.
/// Uses AGENTOS_HOME env var if set, otherwise ~/.agentos.
std::filesystem::path agentos_home();

/// Ensures the directory tree under `base` exists and seeds default files.
void initialise_home(const std::filesystem::path& base);

} // namespace agentos

#endif // AGENTOS_HOME_INIT_H
