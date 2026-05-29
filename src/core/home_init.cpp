#include "agentos/home_init.h"
#include <cstdlib>
#include <fstream>
#include <spdlog/spdlog.h>

namespace agentos {

namespace {

const char* PLANNING_ADVISER_DEFAULT = R"(
# planning adviser stub
def plan(goal):
    return []
)";

const char* CODE_WRITER_DEFAULT = R"(
# code-writer adviser stub
def write_code(spec):
    return ""
)";

const char* CODE_REVIEWER_DEFAULT = R"(
# code-reviewer adviser stub
def review(code):
    return []
)";

void write_default_config(const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out) {
        spdlog::error("[home_init] cannot create default config at {}", path.string());
        return;
    }
    out << "# AgentOS user configuration\n";
    out << "# See ADR-013 for format details.\n";
    out << "\n";
    out << "[llm]\n";
    out << "base_url = \"https://api.anthropic.com\"\n";
    out << "api_key = \"\"\n";
    out << "model = \"gpt-4\"\n";
    out << "max_tokens = 1024\n";
    out << "timeout_s = 120\n";
    out << "max_concurrent = 0\n";
    out << "\n";
    out << "[daemon]\n";
    out << "log_level = \"info\"\n";
    out << "max_concurrent_jobs = 4\n";
    out.close();
}

void seed_if_absent(const std::filesystem::path& path, const char* content) {
    if (std::filesystem::exists(path)) {
        return;
    }
    std::ofstream out(path);
    if (!out) {
        spdlog::error("[home_init] cannot seed file {}", path.string());
        return;
    }
    out << content;
    out.close();
    spdlog::info("[home_init] seeded default {}", path.string());
}

} // anonymous namespace

std::filesystem::path agentos_home() {
    if (const char* env = std::getenv("AGENTOS_HOME")) {
        return std::filesystem::path(env);
    }
    const char* home = std::getenv("HOME");
    if (!home) {
        spdlog::error("[home_init] HOME environment variable not set, using /tmp/.agentos");
        return std::filesystem::path("/tmp/.agentos");
    }
    return std::filesystem::path(home) / ".agentos";
}

void initialise_home(const std::filesystem::path& base) {
    const std::vector<std::filesystem::path> dirs = {
        base,
        base / "run",
        base / "advisers" / "planning",
        base / "advisers" / "code-writer",
        base / "advisers" / "code-reviewer",
        base / "workers",
        base / "skills",
        base / "forge",
        base / "logs",
    };

    for (const auto& dir : dirs) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            spdlog::error("[home_init] cannot create directory {}: {}", dir.string(), ec.message());
        }
    }

    // Seed default config only if absent
    const auto cfg = base / "config.toml";
    if (!std::filesystem::exists(cfg)) {
        write_default_config(cfg);
    }

    // Seed default adviser scripts only if absent
    seed_if_absent(base / "advisers/planning/adviser.py",    PLANNING_ADVISER_DEFAULT);
    seed_if_absent(base / "advisers/code-writer/adviser.py", CODE_WRITER_DEFAULT);
    seed_if_absent(base / "advisers/code-reviewer/adviser.py", CODE_REVIEWER_DEFAULT);
}

} // namespace agentos
