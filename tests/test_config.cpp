#include <gtest/gtest.h>
#include "agentos/config.hpp"
#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace agentos {

class ConfigTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        tmp_dir = "/tmp/agentos_config_test_" + std::to_string(getpid());
        std::filesystem::create_directories(tmp_dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir);
    }

    std::string write_toml(const std::string& content) {
        std::string path = tmp_dir + "/test.toml";
        std::ofstream file(path);
        file << content;
        file.close();
        return path;
    }
};

TEST_F(ConfigTest, LoadValidConfig) {
    std::string toml = R"(
[llm]
base_url = "https://custom.api.com"
model = "custom-model"
max_tokens = 2048
timeout_s = 60

[forge]
max_attempts = 5
promotion_threshold = 10

[sandbox]
tier1_memory_mb = 512
tier1_cpu_weight = 200
tier1_pid_limit = 64

[database]
path = "/custom/path/db.sqlite"

[logging]
level = "debug"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->llm.base_url, "https://custom.api.com");
    EXPECT_EQ(cfg->llm.model, "custom-model");
    EXPECT_EQ(cfg->llm.max_tokens, 2048);
    EXPECT_EQ(cfg->llm.timeout_s, 60);
    EXPECT_EQ(cfg->forge.max_attempts, 5);
    EXPECT_EQ(cfg->forge.promotion_threshold, 10);
    EXPECT_EQ(cfg->sandbox.memory_mb, 512);
    EXPECT_EQ(cfg->sandbox.cpu_weight, 200);
    EXPECT_EQ(cfg->sandbox.pid_limit, 64);
    EXPECT_EQ(cfg->database.path, "/custom/path/db.sqlite");
    EXPECT_EQ(cfg->logging.level, "debug");
}

TEST_F(ConfigTest, MissingFileReturnsDefaults) {
    std::string path = tmp_dir + "/nonexistent.toml";
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    // Check defaults
    EXPECT_EQ(cfg->llm.base_url, "https://api.anthropic.com");
    EXPECT_EQ(cfg->llm.model, "claude-opus-4-5");
    EXPECT_EQ(cfg->llm.max_tokens, 1024);
    EXPECT_EQ(cfg->llm.timeout_s, 120);
    EXPECT_EQ(cfg->forge.max_attempts, 3);
    EXPECT_EQ(cfg->forge.promotion_threshold, 5);
    EXPECT_EQ(cfg->sandbox.memory_mb, 256);
    EXPECT_EQ(cfg->sandbox.cpu_weight, 100);
    EXPECT_EQ(cfg->sandbox.pid_limit, 32);
    EXPECT_EQ(cfg->database.path, "/var/lib/agentos/agentos.db");
    EXPECT_EQ(cfg->logging.level, "info");
}

TEST_F(ConfigTest, PartialConfigUsesDefaults) {
    std::string toml = R"(
[llm]
base_url = "https://partial.api.com"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->llm.base_url, "https://partial.api.com");
    EXPECT_EQ(cfg->llm.model, "claude-opus-4-5"); // default
    EXPECT_EQ(cfg->forge.max_attempts, 3); // default
}

TEST_F(ConfigTest, InvalidTomlReturnsError) {
    std::string toml = R"(
[llm
base_url = "broken"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    EXPECT_FALSE(cfg.has_value());
    EXPECT_FALSE(error.empty());
}

TEST_F(ConfigTest, EnvVarOverridesApiKey) {
    // Set environment variable
    setenv("AGENTOS_LLM_API_KEY", "test-key-123", 1);
    // Load config (non-existent file)
    std::string path = tmp_dir + "/nonexistent.toml";
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    // Read env var
    bool found = read_env_api_key(*cfg);
    EXPECT_TRUE(found);
    EXPECT_EQ(cfg->llm.api_key, "test-key-123");
    // Clean up
    unsetenv("AGENTOS_LLM_API_KEY");
}

} // namespace agentos
#include <gtest/gtest.h>
#include "agentos/config.hpp"
#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace agentos {

class ConfigTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        tmp_dir = "/tmp/agentos_config_test_" + std::to_string(getpid());
        std::filesystem::create_directories(tmp_dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir);
    }

    std::string write_toml(const std::string& content) {
        std::string path = tmp_dir + "/test.toml";
        std::ofstream file(path);
        file << content;
        file.close();
        return path;
    }
};

TEST_F(ConfigTest, LoadValidConfig) {
    std::string toml = R"(
[llm]
base_url = "https://custom.api.com"
model = "custom-model"
max_tokens = 2048
timeout_s = 60

[forge]
max_attempts = 5
promotion_threshold = 10

[sandbox]
tier1_memory_mb = 512
tier1_cpu_weight = 200
tier1_pid_limit = 64

[database]
path = "/custom/path/db.sqlite"

[logging]
level = "debug"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->llm.base_url, "https://custom.api.com");
    EXPECT_EQ(cfg->llm.model, "custom-model");
    EXPECT_EQ(cfg->llm.max_tokens, 2048);
    EXPECT_EQ(cfg->llm.timeout_s, 60);
    EXPECT_EQ(cfg->forge.max_attempts, 5);
    EXPECT_EQ(cfg->forge.promotion_threshold, 10);
    EXPECT_EQ(cfg->sandbox.memory_mb, 512);
    EXPECT_EQ(cfg->sandbox.cpu_weight, 200);
    EXPECT_EQ(cfg->sandbox.pid_limit, 64);
    EXPECT_EQ(cfg->database.path, "/custom/path/db.sqlite");
    EXPECT_EQ(cfg->logging.level, "debug");
}

TEST_F(ConfigTest, MissingFileReturnsDefaults) {
    std::string path = tmp_dir + "/nonexistent.toml";
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    // Check defaults
    EXPECT_EQ(cfg->llm.base_url, "https://api.anthropic.com");
    EXPECT_EQ(cfg->llm.model, "claude-opus-4-5");
    EXPECT_EQ(cfg->llm.max_tokens, 1024);
    EXPECT_EQ(cfg->llm.timeout_s, 120);
    EXPECT_EQ(cfg->forge.max_attempts, 3);
    EXPECT_EQ(cfg->forge.promotion_threshold, 5);
    EXPECT_EQ(cfg->sandbox.memory_mb, 256);
    EXPECT_EQ(cfg->sandbox.cpu_weight, 100);
    EXPECT_EQ(cfg->sandbox.pid_limit, 32);
    EXPECT_EQ(cfg->database.path, "/var/lib/agentos/agentos.db");
    EXPECT_EQ(cfg->logging.level, "info");
}

TEST_F(ConfigTest, PartialConfigUsesDefaults) {
    std::string toml = R"(
[llm]
base_url = "https://partial.api.com"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->llm.base_url, "https://partial.api.com");
    EXPECT_EQ(cfg->llm.model, "claude-opus-4-5"); // default
    EXPECT_EQ(cfg->forge.max_attempts, 3); // default
}

TEST_F(ConfigTest, InvalidTomlReturnsError) {
    std::string toml = R"(
[llm
base_url = "broken"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    EXPECT_FALSE(cfg.has_value());
    EXPECT_FALSE(error.empty());
}

TEST_F(ConfigTest, EnvVarOverridesApiKey) {
    // Set environment variable
    setenv("AGENTOS_LLM_API_KEY", "test-key-123", 1);
    // Load config (non-existent file)
    std::string path = tmp_dir + "/nonexistent.toml";
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    // Read env var
    bool found = read_env_api_key(*cfg);
    EXPECT_TRUE(found);
    EXPECT_EQ(cfg->llm.api_key, "test-key-123");
    // Clean up
    unsetenv("AGENTOS_LLM_API_KEY");
}

} // namespace agentos
#include <gtest/gtest.h>
#include "agentos/config.hpp"
#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace agentos {

class ConfigTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        tmp_dir = "/tmp/agentos_config_test_" + std::to_string(getpid());
        std::filesystem::create_directories(tmp_dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir);
    }

    std::string write_toml(const std::string& content) {
        std::string path = tmp_dir + "/test.toml";
        std::ofstream file(path);
        file << content;
        file.close();
        return path;
    }
};

TEST_F(ConfigTest, LoadValidConfig) {
    std::string toml = R"(
[llm]
base_url = "https://custom.api.com"
model = "custom-model"
max_tokens = 2048
timeout_s = 60

[forge]
max_attempts = 5
promotion_threshold = 10

[sandbox]
tier1_memory_mb = 512
tier1_cpu_weight = 200
tier1_pid_limit = 64

[database]
path = "/custom/path/db.sqlite"

[logging]
level = "debug"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->llm.base_url, "https://custom.api.com");
    EXPECT_EQ(cfg->llm.model, "custom-model");
    EXPECT_EQ(cfg->llm.max_tokens, 2048);
    EXPECT_EQ(cfg->llm.timeout_s, 60);
    EXPECT_EQ(cfg->forge.max_attempts, 5);
    EXPECT_EQ(cfg->forge.promotion_threshold, 10);
    EXPECT_EQ(cfg->sandbox.memory_mb, 512);
    EXPECT_EQ(cfg->sandbox.cpu_weight, 200);
    EXPECT_EQ(cfg->sandbox.pid_limit, 64);
    EXPECT_EQ(cfg->database.path, "/custom/path/db.sqlite");
    EXPECT_EQ(cfg->logging.level, "debug");
}

TEST_F(ConfigTest, MissingFileReturnsDefaults) {
    std::string path = tmp_dir + "/nonexistent.toml";
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    // Check defaults
    EXPECT_EQ(cfg->llm.base_url, "https://api.anthropic.com");
    EXPECT_EQ(cfg->llm.model, "claude-opus-4-5");
    EXPECT_EQ(cfg->llm.max_tokens, 1024);
    EXPECT_EQ(cfg->llm.timeout_s, 120);
    EXPECT_EQ(cfg->forge.max_attempts, 3);
    EXPECT_EQ(cfg->forge.promotion_threshold, 5);
    EXPECT_EQ(cfg->sandbox.memory_mb, 256);
    EXPECT_EQ(cfg->sandbox.cpu_weight, 100);
    EXPECT_EQ(cfg->sandbox.pid_limit, 32);
    EXPECT_EQ(cfg->database.path, "/var/lib/agentos/agentos.db");
    EXPECT_EQ(cfg->logging.level, "info");
}

TEST_F(ConfigTest, PartialConfigUsesDefaults) {
    std::string toml = R"(
[llm]
base_url = "https://partial.api.com"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->llm.base_url, "https://partial.api.com");
    EXPECT_EQ(cfg->llm.model, "claude-opus-4-5"); // default
    EXPECT_EQ(cfg->forge.max_attempts, 3); // default
}

TEST_F(ConfigTest, InvalidTomlReturnsError) {
    std::string toml = R"(
[llm
base_url = "broken"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    EXPECT_FALSE(cfg.has_value());
    EXPECT_FALSE(error.empty());
}

TEST_F(ConfigTest, EnvVarOverridesApiKey) {
    // Set environment variable
    setenv("AGENTOS_LLM_API_KEY", "test-key-123", 1);
    // Load config (non-existent file)
    std::string path = tmp_dir + "/nonexistent.toml";
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    // Read env var
    bool found = read_env_api_key(*cfg);
    EXPECT_TRUE(found);
    EXPECT_EQ(cfg->llm.api_key, "test-key-123");
    // Clean up
    unsetenv("AGENTOS_LLM_API_KEY");
}

} // namespace agentos
#include <gtest/gtest.h>
#include "agentos/config.hpp"
#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace agentos {

class ConfigTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        tmp_dir = "/tmp/agentos_config_test_" + std::to_string(getpid());
        std::filesystem::create_directories(tmp_dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir);
    }

    std::string write_toml(const std::string& content) {
        std::string path = tmp_dir + "/test.toml";
        std::ofstream file(path);
        file << content;
        file.close();
        return path;
    }
};

TEST_F(ConfigTest, LoadValidConfig) {
    std::string toml = R"(
[llm]
base_url = "https://custom.api.com"
model = "custom-model"
max_tokens = 2048
timeout_s = 60

[forge]
max_attempts = 5
promotion_threshold = 10

[sandbox]
tier1_memory_mb = 512
tier1_cpu_weight = 200
tier1_pid_limit = 64

[database]
path = "/custom/path/db.sqlite"

[logging]
level = "debug"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->llm.base_url, "https://custom.api.com");
    EXPECT_EQ(cfg->llm.model, "custom-model");
    EXPECT_EQ(cfg->llm.max_tokens, 2048);
    EXPECT_EQ(cfg->llm.timeout_s, 60);
    EXPECT_EQ(cfg->forge.max_attempts, 5);
    EXPECT_EQ(cfg->forge.promotion_threshold, 10);
    EXPECT_EQ(cfg->sandbox.memory_mb, 512);
    EXPECT_EQ(cfg->sandbox.cpu_weight, 200);
    EXPECT_EQ(cfg->sandbox.pid_limit, 64);
    EXPECT_EQ(cfg->database.path, "/custom/path/db.sqlite");
    EXPECT_EQ(cfg->logging.level, "debug");
}

TEST_F(ConfigTest, MissingFileReturnsDefaults) {
    std::string path = tmp_dir + "/nonexistent.toml";
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    // Check defaults
    EXPECT_EQ(cfg->llm.base_url, "https://api.anthropic.com");
    EXPECT_EQ(cfg->llm.model, "claude-opus-4-5");
    EXPECT_EQ(cfg->llm.max_tokens, 1024);
    EXPECT_EQ(cfg->llm.timeout_s, 120);
    EXPECT_EQ(cfg->forge.max_attempts, 3);
    EXPECT_EQ(cfg->forge.promotion_threshold, 5);
    EXPECT_EQ(cfg->sandbox.memory_mb, 256);
    EXPECT_EQ(cfg->sandbox.cpu_weight, 100);
    EXPECT_EQ(cfg->sandbox.pid_limit, 32);
    EXPECT_EQ(cfg->database.path, "/var/lib/agentos/agentos.db");
    EXPECT_EQ(cfg->logging.level, "info");
}

TEST_F(ConfigTest, PartialConfigUsesDefaults) {
    std::string toml = R"(
[llm]
base_url = "https://partial.api.com"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->llm.base_url, "https://partial.api.com");
    EXPECT_EQ(cfg->llm.model, "claude-opus-4-5"); // default
    EXPECT_EQ(cfg->forge.max_attempts, 3); // default
}

TEST_F(ConfigTest, InvalidTomlReturnsError) {
    std::string toml = R"(
[llm
base_url = "broken"
)";
    std::string path = write_toml(toml);
    std::string error;
    auto cfg = load_config(path, error);
    EXPECT_FALSE(cfg.has_value());
    EXPECT_FALSE(error.empty());
}

TEST_F(ConfigTest, EnvVarOverridesApiKey) {
    // Set environment variable
    setenv("AGENTOS_LLM_API_KEY", "test-key-123", 1);
    // Load config (non-existent file)
    std::string path = tmp_dir + "/nonexistent.toml";
    std::string error;
    auto cfg = load_config(path, error);
    ASSERT_TRUE(cfg.has_value());
    // Read env var
    bool found = read_env_api_key(*cfg);
    EXPECT_TRUE(found);
    EXPECT_EQ(cfg->llm.api_key, "test-key-123");
    // Clean up
    unsetenv("AGENTOS_LLM_API_KEY");
}

} // namespace agentos
