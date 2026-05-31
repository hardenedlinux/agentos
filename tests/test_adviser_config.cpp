#include "agentos/config.h"
#include "agentos/home_init.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>

// ----------------------------------------------------------------------
// Helper: RAII guard for environment variables used in tests
// ----------------------------------------------------------------------
class EnvGuard {
public:
    EnvGuard(const char* name, const char* value)
        : name_(name), old_(std::getenv(name) ? std::getenv(name) : nullptr) {
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
    }
    ~EnvGuard() {
        if (old_)
            setenv(name_, old_, 1);
        else
            unsetenv(name_);
    }
private:
    const char* name_;
    const char* old_;
};

// ----------------------------------------------------------------------
// Tests for agentos_home()
// ----------------------------------------------------------------------
class AgentosHomeTest : public ::testing::Test {
protected:
    const char* saved_agentos_home_ = nullptr;
    const char* saved_home_ = nullptr;

    void SetUp() override {
        saved_agentos_home_ = std::getenv("AGENTOS_HOME");
        saved_home_ = std::getenv("HOME");
        unsetenv("AGENTOS_HOME");
    }

    void TearDown() override {
        if (saved_agentos_home_)
            setenv("AGENTOS_HOME", saved_agentos_home_, 1);
        else
            unsetenv("AGENTOS_HOME");

        if (saved_home_)
            setenv("HOME", saved_home_, 1);
        else
            unsetenv("HOME");
    }
};

TEST_F(AgentosHomeTest, UsesAgentosHomeEnv) {
    setenv("AGENTOS_HOME", "/custom/path", 1);
    EXPECT_EQ(agentos_home().string(), "/custom/path");
}

TEST_F(AgentosHomeTest, FallsBackToHomeDotAgentos) {
    unsetenv("AGENTOS_HOME");
    setenv("HOME", "/home/user", 1);
    EXPECT_EQ(agentos_home().string(), "/home/user/.agentos");
}

TEST_F(AgentosHomeTest, UsesTmpIfHomeUnset) {
    unsetenv("AGENTOS_HOME");
    unsetenv("HOME");
    EXPECT_EQ(agentos_home().string(), "/tmp/.agentos");
}

// ----------------------------------------------------------------------
// Tests for initialise_home() seeding logic (ADR-018)
// ----------------------------------------------------------------------
class HomeInitSeedingTest : public ::testing::Test {
protected:
    std::filesystem::path root_;

    void SetUp() override {
        char tmpl[] = "/tmp/agentos_home_init_test_XXXXXX";
        char* dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr) << "mkdtemp failed";
        root_ = dir;
    }

    void TearDown() override {
        std::filesystem::remove_all(root_);
    }

    std::filesystem::path path_under(const std::string& rel) const {
        return root_ / rel;
    }
};

TEST_F(HomeInitSeedingTest, CreatesAdviserDirectories) {
    initialise_home(root_);
    EXPECT_TRUE(std::filesystem::is_directory(path_under("advisers/planning")));
    EXPECT_TRUE(std::filesystem::is_directory(path_under("advisers/code-writer")));
    EXPECT_TRUE(std::filesystem::is_directory(path_under("advisers/code-reviewer")));
    EXPECT_TRUE(std::filesystem::is_directory(path_under("run")));
    EXPECT_TRUE(std::filesystem::is_directory(path_under("workers")));
    EXPECT_TRUE(std::filesystem::is_directory(path_under("logs")));
}

TEST_F(HomeInitSeedingTest, SeedsPlanningManifest) {
    initialise_home(root_);
    auto manifest = path_under("advisers/planning/manifest.toml");
    ASSERT_TRUE(std::filesystem::exists(manifest));
    std::ifstream f(manifest);
    ASSERT_TRUE(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("[meta]"), std::string::npos);
    EXPECT_NE(content.find("planning"), std::string::npos);
}

TEST_F(HomeInitSeedingTest, SeedsCodeWriterSkill) {
    initialise_home(root_);
    auto skill = path_under("advisers/code-writer/skill.md");
    ASSERT_TRUE(std::filesystem::exists(skill));
    std::ifstream f(skill);
    ASSERT_TRUE(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("# Code Writer Adviser"), std::string::npos);
    EXPECT_NE(content.find("## Role"), std::string::npos);
}

TEST_F(HomeInitSeedingTest, DoesNotOverwriteExistingFile) {
    auto dir = root_ / "advisers" / "planning";
    std::filesystem::create_directories(dir);
    auto manifest = dir / "manifest.toml";
    {
        std::ofstream out(manifest);
        out << "custom content\n";
    }
    initialise_home(root_);
    // Re‑read after seeding; existing content must be preserved
    std::ifstream f(manifest);
    ASSERT_TRUE(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "custom content\n");
}

TEST_F(HomeInitSeedingTest, SeedsGlobalConfigIfAbsent) {
    // Ensure global config does not exist yet
    auto cfg = root_ / "config.toml";
    ASSERT_FALSE(std::filesystem::exists(cfg));
    initialise_home(root_);
    ASSERT_TRUE(std::filesystem::exists(cfg));
    std::ifstream f(cfg);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("[llm]"), std::string::npos);
}

TEST_F(HomeInitSeedingTest, DoesNotOverwriteGlobalConfig) {
    // Write a custom global config
    auto cfg = root_ / "config.toml";
    {
        std::ofstream out(cfg);
        out << "# custom global config\n";
    }
    initialise_home(root_);
    std::ifstream f(cfg);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "# custom global config\n");
}

// ----------------------------------------------------------------------
// Tests for resolve_adviser_llm() (ADR-018)
// ----------------------------------------------------------------------
class ResolveAdviserLLMTest : public ::testing::Test {
protected:
    std::filesystem::path temp_dir_;   // root of an adviser package
    agentos::Config       global_;

    void SetUp() override {
        char tmpl[] = "/tmp/agentos_llm_resolve_XXXXXX";
        char* dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr) << "mkdtemp failed";
        temp_dir_ = dir;

        // Sensible global defaults
        global_.llm.base_url   = "https://api.global.com";
        global_.llm.model      = "global-model";
        global_.llm.max_tokens = 2048;
        global_.llm.timeout_s  = 90;
        global_.llm.api_key    = "global-key-toml";   // will be overwritten by env resolution
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    // Write a file relative to the temp adviser package root.
    void write_file(const std::filesystem::path& rel, const std::string& content) {
        auto full = temp_dir_ / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full);
        ASSERT_TRUE(out.is_open());
        out << content;
        out.close();
    }

    agentos::ResolvedAdviserConfig resolve() {
        std::string err;
        auto maybe = agentos::resolve_adviser_llm(temp_dir_, global_, err);
        EXPECT_TRUE(maybe.has_value()) << "resolve_adviser_llm returned nullopt: " << err;
        return std::move(maybe.value());
    }
};

// No adviser config → use global values (API key from environment)
TEST_F(ResolveAdviserLLMTest, NoConfigUsesGlobal) {
    EnvGuard guard("AGENTOS_LLM_API_KEY", nullptr);   // ensure no env var
    auto res = resolve();
    EXPECT_EQ(res.llm.base_url,   global_.llm.base_url);
    EXPECT_EQ(res.llm.model,      global_.llm.model);
    EXPECT_EQ(res.llm.max_tokens, global_.llm.max_tokens);
    EXPECT_EQ(res.llm.timeout_s,  global_.llm.timeout_s);
    EXPECT_EQ(res.llm.api_key,    "");   // env not set
    EXPECT_EQ(res.skill_path,     temp_dir_ / "skill.md");
}

TEST_F(ResolveAdviserLLMTest, NoConfigApiKeyFromEnv) {
    EnvGuard guard("AGENTOS_LLM_API_KEY", "env-key-value");
    auto res = resolve();
    EXPECT_EQ(res.llm.api_key, "env-key-value");
    // other fields unchanged
    EXPECT_EQ(res.llm.base_url, global_.llm.base_url);
}

TEST_F(ResolveAdviserLLMTest, OverrideModelAndBaseUrl) {
    write_file("config.toml", R"(
[llm]
model = "custom-model"
base_url = "https://custom.api"
)");
    auto res = resolve();
    EXPECT_EQ(res.llm.model, "custom-model");
    EXPECT_EQ(res.llm.base_url, "https://custom.api");
    // global defaults for other fields
    EXPECT_EQ(res.llm.max_tokens, global_.llm.max_tokens);
    EXPECT_EQ(res.llm.timeout_s,  global_.llm.timeout_s);
}

TEST_F(ResolveAdviserLLMTest, OverrideTimeout) {
    write_file("config.toml", R"(
[llm]
timeout_s = 300
)");
    auto res = resolve();
    EXPECT_EQ(res.llm.timeout_s, 300);
    // model and base_url remain global
    EXPECT_EQ(res.llm.model, global_.llm.model);
    EXPECT_EQ(res.llm.base_url, global_.llm.base_url);
}

TEST_F(ResolveAdviserLLMTest, EmptyModelFallsBackToGlobal) {
    write_file("config.toml", R"(
[llm]
model = ""
)");
    auto res = resolve();
    EXPECT_EQ(res.llm.model, global_.llm.model);
}

TEST_F(ResolveAdviserLLMTest, ApiKeyEnvOverride) {
    write_file("config.toml", R"(
[llm]
api_key_env = "MY_SPECIAL_KEY"
)");
    // set the custom env var as well as AGENTOS_LLM_API_KEY to verify priority
    EnvGuard guard_custom("MY_SPECIAL_KEY", "override-key");
    EnvGuard guard_global("AGENTOS_LLM_API_KEY", "global-key");
    auto res = resolve();
    EXPECT_EQ(res.llm.api_key, "override-key");
}

TEST_F(ResolveAdviserLLMTest, ApiKeyEnvEmptyFallsBack) {
    write_file("config.toml", R"(
[llm]
api_key_env = ""
)");
    EnvGuard guard("AGENTOS_LLM_API_KEY", "fallback-key");
    auto res = resolve();
    EXPECT_EQ(res.llm.api_key, "fallback-key");
}

TEST_F(ResolveAdviserLLMTest, ApiKeyEnvNotSetFallsBack) {
    write_file("config.toml", R"(
[llm]
api_key_env = "MISSING_VAR"
)");
    EnvGuard guard("AGENTOS_LLM_API_KEY", "default-key");
    auto res = resolve();
    // custom env var not present → fallback used
    EXPECT_EQ(res.llm.api_key, "default-key");
}

TEST_F(ResolveAdviserLLMTest, NoApiKeyAtAll) {
    // no env vars and no api_key_env in config
    EnvGuard guard_a("AGENTOS_LLM_API_KEY", nullptr);
    auto res = resolve();
    EXPECT_EQ(res.llm.api_key, "");
}

TEST_F(ResolveAdviserLLMTest, ParseErrorFallsBack) {
    // Malformed TOML file
    write_file("config.toml", "= invalid");
    EnvGuard guard("AGENTOS_LLM_API_KEY", "env-key");
    auto res = resolve();
    // All LLM fields must equal the global defaults because parsing failed
    EXPECT_EQ(res.llm.base_url,   global_.llm.base_url);
    EXPECT_EQ(res.llm.model,      global_.llm.model);
    EXPECT_EQ(res.llm.max_tokens, global_.llm.max_tokens);
    EXPECT_EQ(res.llm.timeout_s,  global_.llm.timeout_s);
    // API key resolution still sees the env var because it is independent of TOML parsing
    EXPECT_EQ(res.llm.api_key,    "env-key");
}

TEST_F(ResolveAdviserLLMTest, SkillPathPointToPackageRoot) {
    auto res = resolve();
    EXPECT_EQ(res.skill_path, temp_dir_ / "skill.md");
}
