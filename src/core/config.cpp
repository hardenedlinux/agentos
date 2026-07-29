#include "agentos/config.h"
#include "agentos/home_init.h"
#include "agentos/memory_curve.h"
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <toml.hpp>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <filesystem>

namespace agentos {

std::optional<Config> load_config(std::string_view path, std::string& error) {
    Config cfg;

    // Determine config file path
    std::filesystem::path config_path;
    if (!path.empty()) {
        config_path = path;
    } else {
        config_path = agentos_home() / "config.toml";
    }

    // Open file
    std::ifstream file(config_path);
    if (!file.is_open()) {
        // File not found is not an error; use defaults
        return cfg;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    try {
        toml::table tbl = toml::parse(content);

        // [llm]
        if (auto* llm_tbl = tbl["llm"].as_table()) {
            if (auto v = llm_tbl->at_path("base_url").as_string()) {
                cfg.llm.base_url = v->get();
            }
            if (auto v = llm_tbl->at_path("model").as_string()) {
                cfg.llm.model = v->get();
            }
            if (auto v = llm_tbl->at_path("max_tokens").as_integer()) {
                cfg.llm.max_tokens = static_cast<int>(v->get());
            }
            if (auto v = llm_tbl->at_path("timeout_s").as_integer()) {
                cfg.llm.timeout_s = static_cast<int>(v->get());
            }
            if (auto v = llm_tbl->at_path("max_concurrent").as_integer()) {
                cfg.llm.max_concurrent = static_cast<int>(v->get());
            }
        }

        // [forge]
        if (auto* forge_tbl = tbl["forge"].as_table()) {
            if (auto v = forge_tbl->at_path("max_attempts").as_integer()) {
                cfg.forge.max_attempts = static_cast<int>(v->get());
            }
            if (auto v = forge_tbl->at_path("promotion_threshold").as_integer()) {
                cfg.forge.promotion_threshold = static_cast<int>(v->get());
            }
        }

        // [sandbox]
        if (auto* sandbox_tbl = tbl["sandbox"].as_table()) {
            if (auto v = sandbox_tbl->at_path("tier1_memory_mb").as_integer()) {
                cfg.sandbox.memory_mb = static_cast<int>(v->get());
            }
            if (auto v = sandbox_tbl->at_path("tier1_cpu_weight").as_integer()) {
                cfg.sandbox.cpu_weight = static_cast<int>(v->get());
            }
            if (auto v = sandbox_tbl->at_path("tier1_pid_limit").as_integer()) {
                cfg.sandbox.pid_limit = static_cast<int>(v->get());
            }
        }

        // [database]
        if (auto* db_tbl = tbl["database"].as_table()) {
            if (auto v = db_tbl->at_path("path").as_string()) {
                cfg.database.path = v->get();
            }
        }

        // [logging]
        if (auto* log_tbl = tbl["logging"].as_table()) {
            if (auto v = log_tbl->at_path("level").as_string()) {
                cfg.logging.level = v->get();
            }
        }

        // [memory_curve]
        if (auto* mc = tbl["memory_curve"].as_table()) {
            for (const auto& [key, sub] : *mc) {
                auto* sub_tbl = sub.as_table();
                if (!sub_tbl) continue;

                Config::MemoryCurveFactConfig cfg_mc;
                cfg_mc.algorithm = sub_tbl->at_path("algorithm").value_or("");

                auto* params_tbl = sub_tbl->at_path("params").as_table();
                if (params_tbl) {
                    rapidjson::StringBuffer buf;
                    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
                    w.StartObject();
                    for (const auto& [pk, pv] : *params_tbl) {
                        w.Key(std::string{pk.str()}.c_str());
                        if (auto s = pv.as_string()) {
                            w.String(s->get().c_str());
                        } else if (auto i = pv.as_integer()) {
                            w.Int64(i->get());
                        } else if (auto f = pv.as_floating_point()) {
                            w.Double(f->get());
                        } else if (auto b = pv.as_boolean()) {
                            w.Bool(b->get());
                        } else {
                            w.Null();
                        }
                    }
                    w.EndObject();
                    cfg_mc.params_json = buf.GetString();
                } else {
                    cfg_mc.params_json = "{}";
                }

                cfg.memory_curve.emplace(std::string{key.str()},
                                           std::move(cfg_mc));
            }

            // validate algorithm names (ADR-036 fail-fast: confirmed
            // working via a debug session — an unregistered algorithm
            // name correctly returns nullopt here, which main.cpp's
            // `if (!config) die(...)` correctly refuses to start on).
            for (const auto& [fact_type, mc_cfg] : cfg.memory_curve) {
                auto algo = MemoryCurveRegistry::instance().resolve(mc_cfg.algorithm);
                if (!algo) {
                    error = "memory_curve." + fact_type + ": algorithm '" +
                            mc_cfg.algorithm + "' is not registered";
                    return std::nullopt;
                }
            }
        }

    } catch (const toml::parse_error& e) {
        error = e.what();
        return std::nullopt;
    }

    return cfg;
}

bool read_env_api_key(Config& cfg) {
    const char* val = std::getenv("AGENTOS_LLM_API_KEY");
    if (val) {
        cfg.llm.api_key = val;
        // Clear env var
        unsetenv("AGENTOS_LLM_API_KEY");
        return true;
    }
    return false;
}

// ADR-018: resolution of adviser LLM configuration
std::optional<ResolvedAdviserConfig> resolve_adviser_llm(
    const std::filesystem::path& adviser_dir,
    const Config&                global,
    std::string&                 error) {

    // Start with the daemon global LLM defaults.
    Config::Llm resolved = global.llm;

    const auto cfg_path = adviser_dir / "config.toml";

    // Variables that are only obtained from the adviser config file.
    std::string api_key_env_override;

    if (std::filesystem::exists(cfg_path) && std::filesystem::is_regular_file(cfg_path)) {
        try {
            std::ifstream file(cfg_path);
            std::string content;
            if (file.is_open()) {
                std::stringstream ss;
                ss << file.rdbuf();
                content = ss.str();
            }
            if (!content.empty()) {
                toml::table tbl = toml::parse(content);

                if (auto* llm = tbl["llm"].as_table()) {
                    // Fields that override the global values only if present and non‑empty.
                    if (auto v = llm->at_path("model").as_string()) {
                        if (!v->get().empty()) {
                            resolved.model = v->get();
                        }
                    }
                    if (auto v = llm->at_path("base_url").as_string()) {
                        if (!v->get().empty()) {
                            resolved.base_url = v->get();
                        }
                    }
                    if (auto v = llm->at_path("max_tokens").as_integer()) {
                        resolved.max_tokens = static_cast<int>(v->get());
                    }
                    if (auto v = llm->at_path("timeout_s").as_integer()) {
                        resolved.timeout_s = static_cast<int>(v->get());
                    }
                    if (auto v = llm->at_path("api_key_env").as_string()) {
                        api_key_env_override = v->get();
                    }
                }
            }
        } catch (const toml::parse_error&) {
            // Parsing error – fall through to global defaults.
        }
    }

    // Resolve API key: start with global value (already resolved by daemon).
    std::string api_key = resolved.api_key;

    // If the adviser config specifies a dedicated env‑var name, try to read it.
    if (!api_key_env_override.empty()) {
        const char* env = std::getenv(api_key_env_override.c_str());
        if (env && env[0] != '\0') {
            api_key = env;
        }
    }

    resolved.api_key = api_key;

    ResolvedAdviserConfig result;
    result.llm        = resolved;
    result.skill_path = adviser_dir / "skill.md";

    return result;
}

} // namespace agentos
