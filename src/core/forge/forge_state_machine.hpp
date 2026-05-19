#ifndef AGENTOS_FORGE_STATE_MACHINE_HPP
#define AGENTOS_FORGE_STATE_MACHINE_HPP

#include <string>
#include <functional>
#include "forge_job.hpp"

namespace agentos {

class ForgeStateMachine {
public:
    // Callbacks for each stage
    using DraftCallback = std::function<void(ForgeJob&)>;
    using ReviewCallback = std::function<void(ForgeJob&)>;
    using SandboxProbeCallback = std::function<void(ForgeJob&)>;
    using ApproveCallback = std::function<void(ForgeJob&)>;
    using PromoteCallback = std::function<void(ForgeJob&)>;
    using HumanReviewCallback = std::function<void(ForgeJob&)>;

    ForgeStateMachine(DraftCallback draft,
                      ReviewCallback review,
                      SandboxProbeCallback probe,
                      ApproveCallback approve,
                      PromoteCallback promote,
                      HumanReviewCallback humanReview);

    void process(ForgeJob& job);
    // Future: use EnforceLayer for deterministic state transitions

private:
    DraftCallback draft_;
    ReviewCallback review_;
    SandboxProbeCallback probe_;
    ApproveCallback approve_;
    PromoteCallback promote_;
    HumanReviewCallback humanReview_;
};

} // namespace agentos

#endif // AGENTOS_FORGE_STATE_MACHINE_HPP
