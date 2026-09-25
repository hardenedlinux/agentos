/**
 * Copyright (C) 2026  HardenedLinux community
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <gtest/gtest.h>

#include <rapidjson/document.h>
#include <cmath>
#include <optional>

#include "agentos/memory_curve.h"

using namespace agentos;

namespace {
// register_builtin_memory_curve_algorithms() replaced the old
// self-registering-static-object pattern (that pattern could get its
// translation unit silently dropped by the linker when built into a
// static library — see memory-curve-algorithm.md). Call it explicitly
// once before any test runs; safe to call more than once.
struct EnsureAlgorithmsRegistered {
    EnsureAlgorithmsRegistered() { register_builtin_memory_curve_algorithms(); }
} _ensure_algorithms_registered;
}  // namespace

TEST(MemoryCurveTest, ResolveEmaExists)
{
    auto fn = MemoryCurveRegistry::instance().resolve("ema");
    ASSERT_TRUE(fn.has_value());
}

TEST(MemoryCurveTest, ResolveUnknownReturnsNullopt)
{
    auto fn = MemoryCurveRegistry::instance().resolve("not_a_real_algorithm");
    EXPECT_FALSE(fn.has_value());
}

TEST(MemoryCurveTest, EmaFirstSignalNoOldScore)
{
    auto fn = MemoryCurveRegistry::instance().resolve("ema");
    ASSERT_TRUE(fn.has_value());

    rapidjson::Document params;
    params.SetObject();
    params.AddMember("alpha", 0.25, params.GetAllocator());

    const double signal = 0.8;
    ScoreUpdateInput input{std::nullopt, signal};
    double result = (*fn)(input, params);
    EXPECT_DOUBLE_EQ(result, signal);
}

TEST(MemoryCurveTest, EmaBlend)
{
    auto fn = MemoryCurveRegistry::instance().resolve("ema");
    ASSERT_TRUE(fn.has_value());

    rapidjson::Document params;
    params.SetObject();
    params.AddMember("alpha", 0.25, params.GetAllocator());

    ScoreUpdateInput input{0.5, 1.0};
    double result = (*fn)(input, params);
    // 0.5*(1-0.25) + 1.0*0.25 = 0.625
    EXPECT_NEAR(result, 0.625, 1e-9);
}

TEST(MemoryCurveTest, EmaAlphaZeroKeepsOldScore)
{
    auto fn = MemoryCurveRegistry::instance().resolve("ema");
    ASSERT_TRUE(fn.has_value());

    rapidjson::Document params;
    params.SetObject();
    params.AddMember("alpha", 0.0, params.GetAllocator());

    ScoreUpdateInput input{0.3, 1.0};
    double result = (*fn)(input, params);

    EXPECT_DOUBLE_EQ(result, 0.3);
}

TEST(MemoryCurveTest, EmaAlphaOneUsesSignal)
{
    auto fn = MemoryCurveRegistry::instance().resolve("ema");
    ASSERT_TRUE(fn.has_value());

    rapidjson::Document params;
    params.SetObject();
    params.AddMember("alpha", 1.0, params.GetAllocator());

    ScoreUpdateInput input{0.3, 0.9};
    double result = (*fn)(input, params);

    EXPECT_DOUBLE_EQ(result, 0.9);
}
