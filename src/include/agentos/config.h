#pragma once
#include <string>
#include <optional>
#include <string_view>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace agentos {

struct Config {
    struct Llm {
        std::string base_url   = "https://api.anthropic.com";
        std::string model      = "claude-opus-4-5";
        int         max_tokens = 1024;
        int         timeout_s  = 120;
        std::string api_key;   // from env, not TOML
        int         max_concurrent = 0;   // 0 = auto (kDefaultLlmConcurrency, see llm_proxy.h);
                                          // pool threads block on network I/O, not CPU, so
                                          // this is sized to typical provider concurrency
                                          // limits rather than the host's core count

        // ADR-017 (DeepSeek-specific 429 handling): cap on total time spent
        // retrying a single request that hit DeepSeek's concurrency-limit
        // 429. Bounded by elapsed wall time, not attempt count -- see
        // llm_proxy.cpp's retry_deepseek_429(). Has no effect on non-DeepSeek
        // base_urls, whose 4xx responses (429 included) are never retried.
        int         rate_limit_max_wait_s = 600;
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
        std::string path = "/var/lib/agentos/agentos.db";   // keep for backward‑compatibility; actual path resolved at startup
    } database;

    struct Logging {
        std::string level = "info";
    } logging;

    struct Gateway {
        int heartbeat_interval_s = 30;
        int rate_limit_per_key   = 10;
    } gateway;

    // ADR-XXX: Trusted Worker Network Exemption.
    //
    // Operator-controlled, config-file-only list of registered Worker
    // *names* exempt from the network sandbox restriction (Landlock
    // LANDLOCK_ACCESS_NET_CONNECT_TCP / CLONE_NEWNET). This grant can
    // NEVER be declared by a Worker's own manifest.json/manifest.toml —
    // if it could, any Suite would be able to self-certify its own
    // trust, which defeats the point of the mechanism. It is read once
    // at daemon startup from the daemon's own config.toml and never
    // updated at runtime (no RPC path writes to it) — changing the
    // trust set requires editing the file and restarting the daemon,
    // matching the Enforce Layer's "statically enumerable" principle.
    //
    // Absence of a [trusted_workers] section, or an empty
    // network_exempt array, means no Worker is exempt — this is the
    // default, fail-closed state.
    //
    // Exemption is intersected with the Worker's own declared `network`
    // grant (RegisteredExecutor::network) at dispatch time — being on
    // this list raises the ceiling, it does not substitute for the
    // Worker's own manifest declaration. See
    // Orchestrator::dispatch_next_step.
    //
    // Known limitation: matching is by registered name only. AgentOS
    // does not yet have a reliable field to distinguish a hand-authored/
    // Suite-bundled Worker from a Forge-generated one (the agent ID
    // namespacing backlog — local:/forge:/marketplace: prefixes — is not
    // implemented yet). Until that lands, this list must never contain
    // a name that a Forge-generated Worker could plausibly be given;
    // Forge-generated Worker names are not operator-chosen, so this is a
    // weak but practical guarantee, not an enforced one.
    struct TrustedWorkers {
        std::vector<std::string> network_exempt;
    } trusted_workers;

    // ADR-028: Credential vault configuration
    struct Vault {
        std::string tier               = "community"; // community | standard | enterprise
        int         refresh_ahead_s    = 300;          // refresh when < N seconds remain
        int         refresh_poll_interval = 60;        // seconds between refresh scans
        std::vector<int> pcr_indices   = {0, 1, 2, 7}; // enterprise PCR selection
    } vault;

    struct MemoryCurveFactConfig {
        std::string algorithm;
        std::string params_json;
    };
    std::unordered_map<std::string, MemoryCurveFactConfig> memory_curve;
};

// Load config from TOML file. Returns nullopt on error, with error message in `error`.
[[nodiscard]]
std::optional<Config> load_config(std::string_view path, std::string& error);

// Read AGENTOS_LLM_API_KEY from environment and set cfg.llm.api_key.
// Returns true if the env var was set, false otherwise.
bool read_env_api_key(Config& cfg);

// ADR-018: Resolved configuration for a single adviser after merging with global config.
struct ResolvedAdviserConfig {
    Config::Llm                 llm;
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
