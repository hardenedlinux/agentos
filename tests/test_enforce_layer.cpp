#include <gtest/gtest.h>
#include "agentos/enforce_layer.h"
#include "agentos/registry.h"
#include "database/database.hpp"

namespace agentos {
namespace {

class EnforceLayerTest : public ::testing::Test {
protected:
    // Mock Registry and Database
    class MockRegistry : public Registry {
    public:
        // No need to implement anything for these tests
    };

    class MockDatabase : public Database {
    public:
        MockDatabase() : Database(":memory:") {}
    };

    MockRegistry registry;
    MockDatabase db;
    EnforceLayer enforceLayer{registry, db};
};

TEST_F(EnforceLayerTest, CapabilityAllowed_NetworkFalse_ReturnsTrue) {
    CapabilityDeclaration decl;
    decl.network = false;
    decl.exec = false;
    EXPECT_TRUE(enforceLayer.capability_allowed(decl, "/tmp/job"));
}

TEST_F(EnforceLayerTest, CapabilityAllowed_NetworkTrue_ReturnsFalse) {
    CapabilityDeclaration decl;
    decl.network = true;
    decl.exec = false;
    EXPECT_FALSE(enforceLayer.capability_allowed(decl, "/tmp/job"));
}

TEST_F(EnforceLayerTest, CapabilityAllowed_ExecTrue_ReturnsFalse) {
    CapabilityDeclaration decl;
    decl.network = false;
    decl.exec = true;
    EXPECT_FALSE(enforceLayer.capability_allowed(decl, "/tmp/job"));
}

TEST_F(EnforceLayerTest, EvaluateResources_ReturnsNonZero) {
    auto usage = enforceLayer.evaluate_resources();
    EXPECT_GT(usage.mem_total_kb, 0);
    EXPECT_GT(usage.mem_available_kb, 0);
    // cgroup values may be zero if not in a container
}

TEST_F(EnforceLayerTest, CanTransition_ValidTransition_ReturnsTrue) {
    EXPECT_TRUE(enforceLayer.can_transition("Drafting", "Reviewing"));
    EXPECT_TRUE(enforceLayer.can_transition("Reviewing", "SandboxProbe"));
    EXPECT_TRUE(enforceLayer.can_transition("SandboxProbe", "Approved"));
    EXPECT_TRUE(enforceLayer.can_transition("Approved", "Promoted"));
    EXPECT_TRUE(enforceLayer.can_transition("Promoted", "Done"));
}

TEST_F(EnforceLayerTest, CanTransition_InvalidTransition_ReturnsFalse) {
    EXPECT_FALSE(enforceLayer.can_transition("Drafting", "Approved"));
    EXPECT_FALSE(enforceLayer.can_transition("Reviewing", "Promoted"));
    EXPECT_FALSE(enforceLayer.can_transition("SandboxProbe", "Drafting"));
}

TEST_F(EnforceLayerTest, ValidateSandboxProbe_EmptyCode_ReturnsFalse) {
    EXPECT_FALSE(enforceLayer.validate_sandbox_probe("job1", ""));
}

TEST_F(EnforceLayerTest, ValidateSandboxProbe_NonEmptyCode_ReturnsTrue) {
    EXPECT_TRUE(enforceLayer.validate_sandbox_probe("job1", "int main() {}"));
}

TEST_F(EnforceLayerTest, HumanEscalationRequired_AttemptExceedsMax_ReturnsTrue) {
    EXPECT_TRUE(enforceLayer.human_escalation_required("job1", 4, 3));
}

TEST_F(EnforceLayerTest, HumanEscalationRequired_AttemptWithinMax_ReturnsFalse) {
    EXPECT_FALSE(enforceLayer.human_escalation_required("job1", 2, 3));
}

} // namespace
} // namespace agentos
