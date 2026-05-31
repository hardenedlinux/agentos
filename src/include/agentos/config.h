#pragma once
#include <string>
#include <optional>
#include <string_view>
#include <filesystem>

namespace agentos {

struct Config {
    struct Llm {
        std::string base_url   = "https://api.anthropic.com";
        std::string model      = "claude-opus-4-5";
        int         max_tokens = 1024;
        int         timeout_s  = 120;
        std::string api_key;   // from env, not TOML
        int         max_concurrent = 0;   // 0 = auto (hardware_concurrency - 1, min 1)
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

// ADR-018: Resolved configuration for a single adviser after merging with global config.
struct ResolvedAdviserConfig {
    Llm                         llm;
    std::filesystem::path       skill_path;
};

// ADR-018: Resolve effective LLM configuration for an adviser.
// `adviser_dir` is the root of the adviser package (contains manifest.toml, skill.md, config.toml).
// `global` is the daemon's global config (already loaded).
// Returns the fully‑resolved LLM block for the adviser together with the path to skill.md.
// If adviser config.toml cannot be parsed, the function falls back to global defaults.
[[nodiscard]]
std::optional<ResolvedAdviserConfig> resolve_adviser_llm(
    const std::filesystem::path& adviser_dir,
    const Config&                global,
    std::string&                 error);

} // namespace agentos
