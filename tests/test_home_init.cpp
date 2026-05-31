#include "agentos/home_init.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <string>

class HomeInitTest : public ::testing::Test {
protected:
    std::filesystem::path root_;

    void SetUp() override {
        char tmpl[] = "/tmp/agentos_test_XXXXXX";
        char* dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr) << "mkdtemp failed";
        root_ = dir;
        agentos::initialise_home(root_);
    }

    void TearDown() override {
        std::filesystem::remove_all(root_);
    }
};

TEST_F(HomeInitTest, initialise_home_creates_directories) {
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "advisers" / "planning"));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "advisers" / "code-writer"));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "advisers" / "code-reviewer"));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "run"));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "workers"));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "skills"));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "forge"));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "logs"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "config.toml"));
}

TEST_F(HomeInitTest, initialise_home_seeds_adviser_scripts) {
    auto advisers_dir = root_ / "advisers";
    // planning
    ASSERT_TRUE(std::filesystem::exists(advisers_dir / "planning" / "adviser.py"));
    ASSERT_TRUE(std::filesystem::exists(advisers_dir / "planning" / "planning.py"));
    // code-writer
    ASSERT_TRUE(std::filesystem::exists(advisers_dir / "code-writer" / "adviser.py"));
    ASSERT_TRUE(std::filesystem::exists(advisers_dir / "code-writer" / "code_writer.py"));
    // code-reviewer
    ASSERT_TRUE(std::filesystem::exists(advisers_dir / "code-reviewer" / "adviser.py"));
    ASSERT_TRUE(std::filesystem::exists(advisers_dir / "code-reviewer" / "code_reviewer.py"));
}

TEST_F(HomeInitTest, initialise_home_seeds_skill_files) {
    auto advisers_dir = root_ / "advisers";
    EXPECT_TRUE(std::filesystem::exists(advisers_dir / "planning" / "skill.md"));
    EXPECT_TRUE(std::filesystem::exists(advisers_dir / "code-writer" / "skill.md"));
    EXPECT_TRUE(std::filesystem::exists(advisers_dir / "code-reviewer" / "skill.md"));
}

TEST_F(HomeInitTest, does_not_overwrite_existing) {
    auto dir = root_ / "advisers" / "planning";
    std::filesystem::create_directories(dir);
    auto path = dir / "adviser.py";
    {
        std::ofstream out(path);
        out << "existing content\n";
    }
    // Re-initialise; existing file should be preserved
    agentos::initialise_home(root_);
    std::ifstream f(path);
    ASSERT_TRUE(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "existing content\n");
}
