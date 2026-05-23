#include "forge_state_machine.h"
#include "agentos/sandbox.h"
#include <spdlog/spdlog.h>

namespace agentos {

ForgeStateMachine::ForgeStateMachine(DraftCallback draft,
                                     ReviewCallback review,
                                     SandboxProbeCallback probe,
                                     ApproveCallback approve,
                                     PromoteCallback promote,
                                     HumanReviewCallback humanReview)
    : draft_(std::move(draft))
    , review_(std::move(review))
    , probe_(std::move(probe))
    , approve_(std::move(approve))
    , promote_(std::move(promote))
    , humanReview_(std::move(humanReview))
{}

void ForgeStateMachine::process(ForgeJob& job) {
    if (job.phase == "Drafting") {
        draft_(job);
        job.phase = "Reviewing";
    } else if (job.phase == "Reviewing") {
        review_(job);
        // If review fails, go back to Drafting with feedback
        // For now, assume success and go to SandboxProbe
        job.phase = "SandboxProbe";
    } else if (job.phase == "SandboxProbe") {
        probe_(job);
        // If probe fails, go back to Drafting with feedback
        // For now, assume success and go to Approved
        job.phase = "Approved";
    } else if (job.phase == "Approved") {
        approve_(job);
        job.phase = "Promoted";
    } else if (job.phase == "Promoted") {
        promote_(job);
        // Done
    } else if (job.phase == "HumanReview") {
        humanReview_(job);
        // Wait for human decision
    } else {
        spdlog::warn("[forge_state_machine] unknown phase: {}", job.phase);
    }
}

} // namespace agentos
