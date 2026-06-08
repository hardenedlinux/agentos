/**
 * tests/capability_test.cpp
 *
 * Unit tests for ADR-006 Layer 2 capability validation.
 */
#include "agentos/capability.h"
#include "agentos/types.h"

#include <gtest/gtest.h>

using namespace agentos;

// ---------------------------------------------------------------------------
// validate_capability
//
// Returns std::expected<CapabilityResult, Error>.
// CapabilityResult.verdict is Approve / Reject / Escalate.
// ---------------------------------------------------------------------------

TEST (CapabilityTest, NetworkTrue_Rejected)
{
  CapabilityDeclaration decl;
  decl.network = true;
  auto result = validate_capability (decl, "/tmp/job");
  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->verdict, CapabilityVerdict::Reject);
}

TEST (CapabilityTest, ExecTrue_Rejected)
{
  CapabilityDeclaration decl;
  decl.exec = true;
  auto result = validate_capability (decl, "/tmp/job");
  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->verdict, CapabilityVerdict::Reject);
}

TEST (CapabilityTest, NetworkAndExecBoth_Rejected)
{
  CapabilityDeclaration decl;
  decl.network = true;
  decl.exec = true;
  auto result = validate_capability (decl, "/tmp/job");
  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->verdict, CapabilityVerdict::Reject);
}

TEST (CapabilityTest, Minimal_Approved)
{
  CapabilityDeclaration decl;
  // network=false, exec=false, no fs_read, no fs_write
  auto result = validate_capability (decl, "/tmp/job");
  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->verdict, CapabilityVerdict::Approve);
}

TEST (CapabilityTest, FsReadInsideJobDir_Approved)
{
  CapabilityDeclaration decl;
  decl.fs_read.push_back ("/tmp/job/input.txt");
  auto result = validate_capability (decl, "/tmp/job");
  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->verdict, CapabilityVerdict::Approve);
}

TEST (CapabilityTest, FsReadOutsideJobDir_Escalated)
{
  // ADR-006: fs_read outside job directory → Escalate, not hard Reject
  CapabilityDeclaration decl;
  decl.fs_read.push_back ("/etc/passwd");
  auto result = validate_capability (decl, "/tmp/job");
  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->verdict, CapabilityVerdict::Escalate);
}

TEST (CapabilityTest, FsReadRelativeInside_Approved)
{
  CapabilityDeclaration decl;
  decl.fs_read.push_back ("input.txt");
  auto result = validate_capability (decl, "/tmp/job");
  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->verdict, CapabilityVerdict::Approve);
}

TEST (CapabilityTest, FsReadRelativeOutside_Escalated)
{
  CapabilityDeclaration decl;
  decl.fs_read.push_back ("../etc/passwd");
  auto result = validate_capability (decl, "/tmp/job");
  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->verdict, CapabilityVerdict::Escalate);
}

TEST (CapabilityTest, TcpConnectPorts_NetworkFalse_Rejected)
{
  // ADR-015: tcp_connect_ports + network:false is mutually exclusive
  CapabilityDeclaration decl;
  decl.network = false;
  decl.tcp_connect_ports.push_back (443);
  auto result = validate_capability (decl, "/tmp/job");
  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->verdict, CapabilityVerdict::Reject);
}

TEST (CapabilityTest, EmptyJobDir_AbsolutePath_Escalated)
{
  CapabilityDeclaration decl;
  decl.fs_read.push_back ("/tmp/job/input.txt");
  auto result = validate_capability (decl, "");
  ASSERT_TRUE (result.has_value ());
  EXPECT_EQ (result->verdict, CapabilityVerdict::Escalate);
}

// ---------------------------------------------------------------------------
// determine_tier
//
// ADR-006: Tier-0 = pre-approved catalog worker (forge_generated=false)
//          Tier-1 = forge-generated worker       (forge_generated=true)
// ---------------------------------------------------------------------------

TEST (CapabilityTest, DetermineTier_ForgeGenerated_Tier1)
{
  EXPECT_EQ (determine_tier (true), SandboxTier::Tier1);
}

TEST (CapabilityTest, DetermineTier_PreApproved_Tier0)
{
  EXPECT_EQ (determine_tier (false), SandboxTier::Tier0);
}
