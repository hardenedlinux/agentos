// Copyright (C) 2026  HardenedLinux community
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// Suppresses spdlog output for the duration of every test run in this
// binary. Several tests (e.g. EnforceLayerTest.ValidateSandboxProbe_*)
// intentionally exercise rejection paths that log at warning/error level —
// that is expected behavior being verified by the test, not a real failure.
// Left unmuted, those lines are easy to mistake for test failures when
// scanning terminal/CI output. This does not affect any assertion logic.
//
// Linked into every test executable via the test_common object library in
// tests/CMakeLists.txt, so it takes effect uniformly across all test
// binaries without each test file needing to do anything.

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

namespace {

class QuietLoggingEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        previous_level_ = spdlog::default_logger()->level();
        spdlog::set_level(spdlog::level::off);
    }

    void TearDown() override {
        spdlog::set_level(previous_level_);
    }

private:
    spdlog::level::level_enum previous_level_ = spdlog::level::info;
};

// Registered during static initialization (before main()/RUN_ALL_TESTS()
// in this binary); gtest calls SetUp() on all registered Environments
// before running any TEST(), and TearDown() after all tests finish.
::testing::Environment* const g_quiet_logging_env =
    ::testing::AddGlobalTestEnvironment(new QuietLoggingEnvironment());

}  // namespace
