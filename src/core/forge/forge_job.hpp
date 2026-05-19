#ifndef AGENTOS_FORGE_JOB_HPP
#define AGENTOS_FORGE_JOB_HPP

#include <string>
#include <cstdint>

namespace agentos {

struct ForgeJob {
    std::string id;
    std::string method;
    std::string requirement;
    int attempt = 0;
    int max_attempts = 3;
    std::string phase; // Drafting, Reviewing, SandboxProbe, Approved, Promoted, HumanReview
    std::string last_code;
    std::string last_feedback;
    int64_t created_at;
    int64_t updated_at;
};

} // namespace agentos

#endif // AGENTOS_FORGE_JOB_HPP
