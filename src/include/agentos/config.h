#pragma once
#include <string>
#include <optional>
#include <string_view>

namespace agentos {

struct Config {
    struct Llm {
        std::string base_url   = "https://api.anthropic.com";
        std::string model      = "claude-opus-4-5";
        int         max_tokens = 1024;
        int         timeout_s  = 120;
        std::string api_key;   // from env, not TOML
    } llm;

    struct Forge {
        int max_attempts        = 3;
        int promotion_threshold = 5;
    } forge;

    struct Sandbox {
        int memory_mb  = 256;
        int cpu_weight = 100;
        int pid_limit  = 32;
    } sandbox;

    struct Database {
        std::string path = "/var/lib/agentos/agentos.db";
    } database;

    struct Logging {
        std::string level = "info";
    } logging;
};

// Load config from TOML file. Returns nullopt on error, with error message in `error`.
[[nodiscard]]
std::optional<Config> load_config(std::string_view path, std::string& error);

// Read AGENTOS_LLM_API_KEY from environment and set cfg.llm.api_key.
// Returns true if the env var was set, false otherwise.
bool read_env_api_key(Config& cfg);

} // namespace agentos
