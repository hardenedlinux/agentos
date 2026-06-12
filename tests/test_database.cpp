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
#include "agentos/database.h"
#include <filesystem>
#include <gtest/gtest.h>

using namespace agentos;

class DatabaseTest : public ::testing::Test
{
protected:
  std::string db_path;
  std::unique_ptr<Database> db;

  void SetUp () override
  {
    db_path = "/tmp/agentos_test_db_" + std::to_string (getpid ()) + ".db";
    db = std::make_unique<Database> (db_path);
    ASSERT_TRUE (db->open ());
  }

  void TearDown () override
  {
    db->close ();
    std::filesystem::remove (db_path);
  }
};

TEST_F (DatabaseTest, OpenAndClose)
{
  EXPECT_TRUE (db->is_open ());
  db->close ();
  EXPECT_FALSE (db->is_open ());
}

TEST_F (DatabaseTest, StoreAndLoadJob)
{
  Task task;
  task.id = "job1";
  task.goal = "test goal";
  task.input_json = R"({"key":"value"})";
  db->store_job (task);
  db->update_job_phase ("job1", "planning");
  std::string plan_json = R"({"steps":[]})";
  db->update_job_plan ("job1", plan_json);
  std::string loaded = db->load_plan_json ("job1");
  EXPECT_EQ (loaded, plan_json);
}

TEST_F (DatabaseTest, ResumeInFlight)
{
  Task task;
  task.id = "job1";
  task.goal = "test";
  db->store_job (task);
  std::string plan_json = R"({"steps":[]})";
  db->update_job_plan ("job1", plan_json);
  auto jobs = db->resume_in_flight ();
  ASSERT_EQ (jobs.size (), 1);
  EXPECT_EQ (jobs[0].job_id, "job1");
  EXPECT_EQ (jobs[0].plan_json, plan_json);
}

TEST_F (DatabaseTest, ResumeInFlightNone)
{
  auto jobs = db->resume_in_flight ();
  EXPECT_TRUE (jobs.empty ());
}
