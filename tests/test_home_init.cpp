#include <gtest/gtest.h>
#include "agentos/home_init.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace agentos {
namespace {

class HomeInitTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Save original env vars
        if (const char* val = std::getenv("AGENTOS_HOME"))
            saved_agentos_home_ = val;
        if (const char* val = std::getenv("HOME"))
            saved_home_ = val;
    }

    void TearDown() override {
        // Restore env vars
        if (saved_agentos_home_.has_value())
            setenv("AGENTOS_HOME", saved_agentos_home_->c_str(), 1);
        else
            unsetenv("AGENTOS_HOME");
        if (saved_home_.has_value())
            setenv("HOME", saved_home_->c_str(), 1);
        else
            unsetenv("HOME");
    }

    std::optional<std::string> saved_agentos_home_;
    std::optional<std::string> saved_home_;
};

TEST_F(HomeInitTest, agentos_home_uses_env_var) {
    unsetenv("HOME");
    setenv("AGENTOS_HOME", "/custom/path", 1);
    EXPECT_EQ(agentos_home(), std::filesystem::path("/custom/path"));
}

TEST_F(HomeInitTest, agentos_home_uses_home_dot_agentos) {
    unsetenv("AGENTOS_HOME");
    setenv("HOME", "/home/user", 1);
    EXPECT_EQ(agentos_home(), std::filesystem::path("/home/user/.agentos"));
}

TEST_F(HomeInitTest, agentos_home_fallback) {
    unsetenv("AGENTOS_HOME");
    unsetenv("HOME");
    EXPECT_EQ(agentos_home(), std::filesystem::path("/tmp/.agentos"));
}

TEST_F(HomeInitTest, initialise_home_creates_directories) {
    // Use a temporary directory
    auto tmp = std::filesystem::temp_directory_path() / "agentos_test_home";
    std::filesystem::remove_all(tmp);

    initialise_home(tmp);

    EXPECT_TRUE(std::filesystem::exists(tmp));
    EXPECT_TRUE(std::filesystem::exists(tmp / "run"));
    EXPECT_TRUE(std::filesystem::exists(tmp / "advisers" / "planning"));
    EXPECT_TRUE(std::filesystem::exists(tmp / "advisers" / "code-writer"));
    EXPECT_TRUE(std::filesystem::exists(tmp / "advisers" / "code-reviewer"));
    EXPECT_TRUE(std::filesystem::exists(tmp / "workers"));
    EXPECT_TRUE(std::filesystem::exists(tmp / "skills"));
    EXPECT_TRUE(std::filesystem::exists(tmp / "forge"));
    EXPECT_TRUE(std::filesystem::exists(tmp / "logs"));

    // Clean up
    std::filesystem::remove_all(tmp);
}

TEST_F(HomeInitTest, initialise_home_seeds_default_config) {
    auto tmp = std::filesystem::temp_directory_path() / "agentos_test_config";
    std::filesystem::remove_all(tmp);

    initialise_home(tmp);

    EXPECT_TRUE(std::filesystem::exists(tmp / "config.toml"));

    // Read content
    std::ifstream in(tmp / "config.toml");
    ASSERT_TRUE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("[llm]"), std::string::npos);
    EXPECT_NE(content.find("[daemon]"), std::string::npos);

    std::filesystem::remove_all(tmp);
}

TEST_F(HomeInitTest, initialise_home_does_not_overwrite_existing_config) {
    auto tmp = std::filesystem::temp_directory_path() / "agentos_test_no_overwrite";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    // Write a custom config
    std::ofstream out(tmp / "config.toml");
    out << "custom = true";
    out.close();

    initialise_home(tmp);

    // Read back – should still be custom
    std::ifstream in(tmp / "config.toml");
    ASSERT_TRUE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "custom = true");

    std::filesystem::remove_all(tmp);
}

TEST_F(HomeInitTest, initialise_home_seeds_adviser_scripts) {
    auto tmp = std::filesystem::temp_directory_path() / "agentos_test_advisers";
    std::filesystem::remove_all(tmp);

    initialise_home(tmp);

    EXPECT_TRUE(std::filesystem::exists(tmp / "advisers/planning/adviser.py"));
    EXPECT_TRUE(std::filesystem::exists(tmp / "advisers/code-writer/adviser.py"));
    EXPECT_TRUE(std::filesystem::exists(tmp / "advisers/code-reviewer/adviser.py"));

    std::filesystem::remove_all(tmp);
}

TEST_F(HomeInitTest, initialise_home_does_not_overwrite_existing_adviser) {
    auto tmp = std::filesystem::temp_directory_path() / "agentos_test_adviser_no_overwrite";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / "advisers" / "planning");

    // Write a custom adviser
    std::ofstream out(tmp / "advisers/planning/adviser.py");
    out << "custom = True";
    out.close();

    initialise_home(tmp);

    // Read back – should still be custom
    std::ifstream in(tmp / "advisers/planning/adviser.py");
    ASSERT_TRUE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "custom = True");

    std::filesystem::remove_all(tmp);
}

} // namespace
} // namespace agentos
