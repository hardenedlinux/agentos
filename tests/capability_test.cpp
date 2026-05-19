/**
 * tests/capability_test.cpp
 *
 * Unit tests for ADR-006 Layer 2 capability validation.
 */

#include <gtest/gtest.h>
#include "agentos/capability.h"
#include "agentos/types.h"

using namespace agentos;

// ---------------------------------------------------------------------------
// validate_capability
// ---------------------------------------------------------------------------

TEST(CapabilityTest, NetworkTrueRejected) {
    CapabilityDeclaration decl;
    decl.network = true;
    EXPECT_FALSE(validate_capability(decl, "/tmp/job"));
}

TEST(CapabilityTest, ExecTrueRejected) {
    CapabilityDeclaration decl;
    decl.exec = true;
    EXPECT_FALSE(validate_capability(decl, "/tmp/job"));
}

TEST(CapabilityTest, NetworkAndExecBothRejected) {
    CapabilityDeclaration decl;
    decl.network = true;
    decl.exec = true;
    EXPECT_FALSE(validate_capability(decl, "/tmp/job"));
}

TEST(CapabilityTest, MinimalAllowed) {
    CapabilityDeclaration decl;
    // network=false, exec=false, no fs_read, no fs_write
    EXPECT_TRUE(validate_capability(decl, "/tmp/job"));
}

TEST(CapabilityTest, FsReadInsideJobDir) {
    CapabilityDeclaration decl;
    decl.fs_read.push_back("/tmp/job/input.txt");
    EXPECT_TRUE(validate_capability(decl, "/tmp/job"));
}

TEST(CapabilityTest, FsReadOutsideJobDir) {
    CapabilityDeclaration decl;
    decl.fs_read.push_back("/etc/passwd");
    EXPECT_FALSE(validate_capability(decl, "/tmp/job"));
}

TEST(CapabilityTest, FsReadRelativeInside) {
    CapabilityDeclaration decl;
    decl.fs_read.push_back("input.txt");
    EXPECT_TRUE(validate_capability(decl, "/tmp/job"));
}

TEST(CapabilityTest, FsReadRelativeOutside) {
    CapabilityDeclaration decl;
    decl.fs_read.push_back("../etc/passwd");
    EXPECT_FALSE(validate_capability(decl, "/tmp/job"));
}

TEST(CapabilityTest, FsWriteNotChecked) {
    CapabilityDeclaration decl;
    decl.fs_write.push_back("/etc/passwd");
    // fs_write is not checked, so should still pass
    EXPECT_TRUE(validate_capability(decl, "/tmp/job"));
}

TEST(CapabilityTest, EmptyJobDir) {
    CapabilityDeclaration decl;
    // job_dir empty, absolute paths should be outside
    decl.fs_read.push_back("/tmp/job/input.txt");
    // relative to empty job_dir, the path is absolute, so relative will be
    // the full path, which is not inside empty dir -> rejected
    EXPECT_FALSE(validate_capability(decl, ""));
}

// ---------------------------------------------------------------------------
// determine_tier
// ---------------------------------------------------------------------------

TEST(CapabilityTest, DetermineTierAlwaysTier1) {
    CapabilityDeclaration decl;
    EXPECT_EQ(determine_tier(decl), SandboxTier::Tier1);
}

TEST(CapabilityTest, DetermineTierWithNetwork) {
    CapabilityDeclaration decl;
    decl.network = true;
    EXPECT_EQ(determine_tier(decl), SandboxTier::Tier1);
}

TEST(CapabilityTest, DetermineTierWithExec) {
    CapabilityDeclaration decl;
    decl.exec = true;
    EXPECT_EQ(determine_tier(decl), SandboxTier::Tier1);
}

TEST(CapabilityTest, DetermineTierWithFsRead) {
    CapabilityDeclaration decl;
    decl.fs_read.push_back("/tmp/job/input.txt");
    EXPECT_EQ(determine_tier(decl), SandboxTier::Tier1);
}
