/**
 * tests/test_enforce_layer.cpp
 *
 * Unit tests for EnforceLayer (ADR-006, ADR-008/019, ADR-009, ADR-011).
 * These are pure unit tests — no LLM calls, no network, no filesystem writes.
 */
#include "agentos/database.h"
#include "agentos/enforce_layer.h"
#include "agentos/registry.h"

#include <gtest/gtest.h>

namespace agentos
{
  namespace
  {

    class EnforceLayerTest : public ::testing::Test
    {
    protected:
      class MockDatabase : public Database
      {
      public:
        MockDatabase () : Database (":memory:") {}
      };

      // Declare db before registry — members initialise in declaration order.
      MockDatabase db;
      Registry registry{db};
      EnforceLayer enforceLayer{registry, db};

      // Helper: build a minimal safe declaration (auto-approve baseline).
      static CapabilityDeclaration safe_decl ()
      {
        CapabilityDeclaration d;
        d.network = false;
        d.exec = false;
        return d;
      }
    };

    // ── capability_allowed
    // ───────────────────────────────────────────────────────

    TEST_F (EnforceLayerTest, CapabilityAllowed_SafeDecl_ReturnsTrue)
    {
      EXPECT_TRUE (enforceLayer.capability_allowed (safe_decl (), "/tmp/job"));
    }

    TEST_F (EnforceLayerTest, CapabilityAllowed_NetworkTrue_ReturnsFalse)
    {
      auto decl = safe_decl ();
      decl.network = true;
      EXPECT_FALSE (enforceLayer.capability_allowed (decl, "/tmp/job"));
    }

    TEST_F (EnforceLayerTest, CapabilityAllowed_ExecTrue_ReturnsFalse)
    {
      auto decl = safe_decl ();
      decl.exec = true;
      EXPECT_FALSE (enforceLayer.capability_allowed (decl, "/tmp/job"));
    }

    // tcp_connect_ports + network:false is mutually exclusive (ADR-015)
    TEST_F (EnforceLayerTest,
            CapabilityAllowed_TcpPortsWithNetworkFalse_ReturnsFalse)
    {
      auto decl = safe_decl ();
      decl.tcp_connect_ports.push_back (443);
      EXPECT_FALSE (enforceLayer.capability_allowed (decl, "/tmp/job"));
    }

    // tcp_connect_ports with network:true should still be rejected
    // (network:true alone triggers rejection before the port check is reached)
    TEST_F (EnforceLayerTest,
            CapabilityAllowed_TcpPortsWithNetworkTrue_ReturnsFalse)
    {
      auto decl = safe_decl ();
      decl.network = true;
      decl.tcp_connect_ports.push_back (443);
      EXPECT_FALSE (enforceLayer.capability_allowed (decl, "/tmp/job"));
    }

    // ── requires_path_escalation
    // ─────────────────────────────────────────────────

    TEST_F (EnforceLayerTest, PathEscalation_PathInsideJobDir_ReturnsFalse)
    {
      auto decl = safe_decl ();
      decl.fs_read.push_back ("/tmp/job/input.txt");
      EXPECT_FALSE (enforceLayer.requires_path_escalation (decl, "/tmp/job"));
    }

    TEST_F (EnforceLayerTest, PathEscalation_PathOutsideJobDir_ReturnsTrue)
    {
      auto decl = safe_decl ();
      decl.fs_read.push_back ("/home/user/secret.txt");
      EXPECT_TRUE (enforceLayer.requires_path_escalation (decl, "/tmp/job"));
    }

    TEST_F (EnforceLayerTest, PathEscalation_TemplatePlaceholder_ReturnsTrue)
    {
      // Template placeholders like {{input_path}} are unresolved at validation
      // time and must trigger escalation (ADR-015).
      auto decl = safe_decl ();
      decl.fs_read.push_back ("{{input_path}}");
      EXPECT_TRUE (enforceLayer.requires_path_escalation (decl, "/tmp/job"));
    }

    TEST_F (EnforceLayerTest, PathEscalation_NoPathsDeclared_ReturnsFalse)
    {
      EXPECT_FALSE (
        enforceLayer.requires_path_escalation (safe_decl (), "/tmp/job"));
    }

    TEST_F (EnforceLayerTest, PathEscalation_FsWriteOutsideDir_ReturnsTrue)
    {
      auto decl = safe_decl ();
      decl.fs_write.push_back ("/etc/passwd");
      EXPECT_TRUE (enforceLayer.requires_path_escalation (decl, "/tmp/job"));
    }

    // ── evaluate_resources
    // ───────────────────────────────────────────────────────

    TEST_F (EnforceLayerTest, EvaluateResources_MemTotalNonZero)
    {
      auto usage = enforceLayer.evaluate_resources ();
      EXPECT_GT (usage.mem_total_kb, 0u);
    }

    TEST_F (EnforceLayerTest, EvaluateResources_MemAvailableNonZero)
    {
      auto usage = enforceLayer.evaluate_resources ();
      EXPECT_GT (usage.mem_available_kb, 0u);
    }

    TEST_F (EnforceLayerTest, EvaluateResources_AvailableLeTotal)
    {
      auto usage = enforceLayer.evaluate_resources ();
      EXPECT_LE (usage.mem_available_kb, usage.mem_total_kb);
    }

    // cgroup values may be 0 outside a cgroup-constrained environment — not
    // tested for non-zero, only for internal consistency.
    TEST_F (EnforceLayerTest, EvaluateResources_CgroupLimitGeUsage)
    {
      auto usage = enforceLayer.evaluate_resources ();
      if (usage.cgroup_mem_limit_kb > 0)
        EXPECT_GE (usage.cgroup_mem_limit_kb, usage.cgroup_mem_usage_kb);
    }

    // ── can_transition (forge state machine, ADR-019)
    // ────────────────────────────

    TEST_F (EnforceLayerTest, CanTransition_DraftingToReviewing_ReturnsTrue)
    {
      EXPECT_TRUE (enforceLayer.can_transition ("drafting", "reviewing"));
    }

    TEST_F (EnforceLayerTest, CanTransition_ReviewingToDrafting_ReturnsTrue)
    {
      EXPECT_TRUE (enforceLayer.can_transition ("reviewing", "drafting"));
    }

    TEST_F (EnforceLayerTest, CanTransition_ReviewingToHumanReview_ReturnsTrue)
    {
      EXPECT_TRUE (enforceLayer.can_transition ("reviewing", "human_review"));
    }

    TEST_F (EnforceLayerTest, CanTransition_ReviewingToPromoted_ReturnsTrue)
    {
      EXPECT_TRUE (enforceLayer.can_transition ("reviewing", "promoted"));
    }

    TEST_F (EnforceLayerTest, CanTransition_HumanReviewToDrafting_ReturnsTrue)
    {
      EXPECT_TRUE (enforceLayer.can_transition ("human_review", "drafting"));
    }

    TEST_F (EnforceLayerTest, CanTransition_HumanReviewToRejected_ReturnsTrue)
    {
      EXPECT_TRUE (enforceLayer.can_transition ("human_review", "rejected"));
    }

    // Invalid transitions
    TEST_F (EnforceLayerTest, CanTransition_DraftingToPromoted_ReturnsFalse)
    {
      EXPECT_FALSE (enforceLayer.can_transition ("drafting", "promoted"));
    }

    TEST_F (EnforceLayerTest, CanTransition_PromotedToAnything_ReturnsFalse)
    {
      // promoted is a terminal state
      EXPECT_FALSE (enforceLayer.can_transition ("promoted", "drafting"));
      EXPECT_FALSE (enforceLayer.can_transition ("promoted", "reviewing"));
      EXPECT_FALSE (enforceLayer.can_transition ("promoted", "rejected"));
    }

    TEST_F (EnforceLayerTest, CanTransition_RejectedToAnything_ReturnsFalse)
    {
      // rejected is a terminal state
      EXPECT_FALSE (enforceLayer.can_transition ("rejected", "drafting"));
      EXPECT_FALSE (enforceLayer.can_transition ("rejected", "reviewing"));
    }

    // Old uppercase states must not be accepted (ADR-019 uses lowercase)
    TEST_F (EnforceLayerTest, CanTransition_UppercaseStates_ReturnsFalse)
    {
      EXPECT_FALSE (enforceLayer.can_transition ("Drafting", "Reviewing"));
      EXPECT_FALSE (enforceLayer.can_transition ("Reviewing", "Promoted"));
    }

    // ── validate_sandbox_probe
    // ───────────────────────────────────────────────────

    TEST_F (EnforceLayerTest, ValidateSandboxProbe_SafeDecl_ReturnsEmpty)
    {
      EXPECT_TRUE (
        enforceLayer.validate_sandbox_probe ("job1", safe_decl ()).empty ());
    }

    TEST_F (EnforceLayerTest, ValidateSandboxProbe_NetworkTrue_ReturnsReason)
    {
      auto decl = safe_decl ();
      decl.network = true;
      EXPECT_FALSE (
        enforceLayer.validate_sandbox_probe ("job1", decl).empty ());
    }

    TEST_F (EnforceLayerTest, ValidateSandboxProbe_ExecTrue_ReturnsReason)
    {
      auto decl = safe_decl ();
      decl.exec = true;
      EXPECT_FALSE (
        enforceLayer.validate_sandbox_probe ("job1", decl).empty ());
    }

    // Reviewer acceptance does NOT bypass Enforce Layer (ADR-009)
    TEST_F (EnforceLayerTest,
            ValidateSandboxProbe_NetworkTrue_ReviewerVerdictIgnored)
    {
      auto decl = safe_decl ();
      decl.network = true;
      // Even if a Reviewer "accepted" this, Enforce Layer must still reject.
      const std::string reason
        = enforceLayer.validate_sandbox_probe ("job-reviewer-bypass", decl);
      EXPECT_FALSE (reason.empty ());
    }

    // ── human_escalation_required
    // ────────────────────────────────────────────────

    TEST_F (EnforceLayerTest, HumanEscalation_AttemptExceedsMax_ReturnsTrue)
    {
      EXPECT_TRUE (enforceLayer.human_escalation_required ("job1", 4, 3));
    }

    TEST_F (EnforceLayerTest, HumanEscalation_AttemptEqualsMax_ReturnsFalse)
    {
      // attempt == max_attempts: still within budget, not yet escalated
      EXPECT_FALSE (enforceLayer.human_escalation_required ("job1", 3, 3));
    }

    TEST_F (EnforceLayerTest, HumanEscalation_AttemptBelowMax_ReturnsFalse)
    {
      EXPECT_FALSE (enforceLayer.human_escalation_required ("job1", 1, 3));
    }

    TEST_F (EnforceLayerTest, HumanEscalation_ZeroAttempt_ReturnsFalse)
    {
      EXPECT_FALSE (enforceLayer.human_escalation_required ("job1", 0, 3));
    }

  } // namespace
} // namespace agentos
