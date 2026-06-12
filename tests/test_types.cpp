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
#include "agentos/types.h"
#include "agentos/uuid.h"

#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <unordered_set>

struct TestTag;

using namespace agentos;

// -----------------------------------------------------------------------
// StrongId tests
// -----------------------------------------------------------------------

TEST (StrongIdTest, DefaultConstructor)
{
  StrongId<struct TestTag> id;
  EXPECT_EQ (id.value (), "");
  EXPECT_STREQ (id.c_str (), "");
}

TEST (StrongIdTest, FromString)
{
  StrongId<struct TestTag> id (std::string ("abc123"));
  EXPECT_EQ (id.value (), "abc123");
  EXPECT_STREQ (id.c_str (), "abc123");
}

TEST (StrongIdTest, FromConstChar)
{
  StrongId<struct TestTag> id ("xyz789");
  EXPECT_EQ (id.value (), "xyz789");
  EXPECT_STREQ (id.c_str (), "xyz789");
}

TEST (StrongIdTest, AssignmentFromString)
{
  StrongId<struct TestTag> id;
  id = std::string ("assigned");
  EXPECT_EQ (id.value (), "assigned");
}

TEST (StrongIdTest, AssignmentFromConstChar)
{
  StrongId<struct TestTag> id;
  id = "cstr";
  EXPECT_EQ (id.value (), "cstr");
}

TEST (StrongIdTest, Equality)
{
  StrongId<struct TestTag> a ("same");
  StrongId<struct TestTag> b ("same");
  StrongId<struct TestTag> c ("diff");
  EXPECT_TRUE (a == b);
  EXPECT_FALSE (a == c);
}

TEST (StrongIdTest, Inequality)
{
  StrongId<struct TestTag> a ("same");
  StrongId<struct TestTag> b ("same");
  StrongId<struct TestTag> c ("diff");
  EXPECT_FALSE (a != b);
  EXPECT_TRUE (a != c);
}

TEST (StrongIdTest, LessThan)
{
  StrongId<struct TestTag> a ("aaa");
  StrongId<struct TestTag> b ("bbb");
  EXPECT_TRUE (a < b);
  EXPECT_FALSE (b < a);
}

TEST (StrongIdTest, StreamOutput)
{
  StrongId<struct TestTag> id ("stream_test");
  std::ostringstream oss;
  oss << id;
  EXPECT_EQ (oss.str (), "stream_test");
}

TEST (StrongIdTest, StringConcatenationLeft)
{
  StrongId<struct TestTag> id ("mid");
  std::string result = "prefix_" + id;
  EXPECT_EQ (result, "prefix_mid");
}

TEST (StrongIdTest, StringConcatenationRight)
{
  StrongId<struct TestTag> id ("mid");
  std::string result = id + "_suffix";
  EXPECT_EQ (result, "mid_suffix");
}

TEST (StrongIdTest, StringConcatenationWithConstChar)
{
  StrongId<struct TestTag> id ("mid");
  std::string result = id + ".sock";
  EXPECT_EQ (result, "mid.sock");
}

TEST (StrongIdTest, ConstCharConcatenationWithStrongId)
{
  StrongId<struct TestTag> id ("mid");
  std::string result = std::string ("/path/") + id;
  EXPECT_EQ (result, "/path/mid");
}

TEST (StrongIdTest, Hash)
{
  std::unordered_set<StrongId<struct TestTag>> set;
  set.insert (StrongId<struct TestTag> ("a"));
  set.insert (StrongId<struct TestTag> ("b"));
  set.insert (StrongId<struct TestTag> ("a")); // duplicate
  EXPECT_EQ (set.size (), 2);
}

// -----------------------------------------------------------------------
// Result tests
// -----------------------------------------------------------------------

TEST (ResultTest, DefaultConstructor)
{
  Result<int> r;
  EXPECT_FALSE (r.ok);
  EXPECT_EQ (r.value, 0);
  EXPECT_EQ (r.error, "");
}

TEST (ResultTest, ValueConstructor)
{
  Result<int> r (42);
  EXPECT_TRUE (r.ok);
  EXPECT_EQ (r.value, 42);
  EXPECT_EQ (r.error, "");
}

TEST (ResultTest, ErrorConstructor)
{
  Result<int> r (std::string ("something went wrong"), ErrorTag{});
  EXPECT_FALSE (r.ok);
  EXPECT_EQ (r.value, 0);
  EXPECT_EQ (r.error, "something went wrong");
}

TEST (ResultTest, StringResult)
{
  Result<std::string> r ("hello");
  EXPECT_TRUE (r.ok);
  EXPECT_EQ (r.value, "hello");
  EXPECT_EQ (r.error, "");
}

TEST (ResultTest, StringError)
{
  Result<std::string> r (std::string ("fail"), ErrorTag{});
  EXPECT_FALSE (r.ok);
  EXPECT_EQ (r.value, "");
  EXPECT_EQ (r.error, "fail");
}

// -----------------------------------------------------------------------
// gen_uuid tests
// -----------------------------------------------------------------------

TEST (UuidTest, ReturnsNonEmpty)
{
  std::string uuid = gen_uuid ();
  EXPECT_FALSE (uuid.empty ());
}

TEST (UuidTest, Length36)
{
  std::string uuid = gen_uuid ();
  EXPECT_EQ (uuid.size (), 36);
}

TEST (UuidTest, Format)
{
  std::string uuid = gen_uuid ();
  // UUID format: 8-4-4-4-12
  EXPECT_EQ (uuid[8], '-');
  EXPECT_EQ (uuid[13], '-');
  EXPECT_EQ (uuid[18], '-');
  EXPECT_EQ (uuid[23], '-');
}

TEST (UuidTest, Uniqueness)
{
  std::string u1 = gen_uuid ();
  std::string u2 = gen_uuid ();
  EXPECT_NE (u1, u2);
}

TEST (UuidTest, HexCharacters)
{
  std::string uuid = gen_uuid ();
  for (size_t i = 0; i < uuid.size (); ++i)
    {
      if (uuid[i] == '-')
        continue;
      EXPECT_TRUE ((uuid[i] >= '0' && uuid[i] <= '9')
                   || (uuid[i] >= 'a' && uuid[i] <= 'f'));
    }
}
