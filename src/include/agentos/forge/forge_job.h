#pragma once

#include <string>
#include <cstdint>
#include <vector>

#include "agentos/types.h"

namespace agentos {

struct ForgeJob {
    ForgeJobId id;
    std::string method;
    std::string requirement;
    int attempt = 0;
    int max_attempts = 3;
    std::string phase; // Drafting, Reviewing, SandboxProbe, Approved, Promoted, HumanReview
    std::string last_code;
    std::string last_feedback;
    int64_t created_at = 0;
    int64_t updated_at = 0;

    // Sandbox configuration (ADR-011)
    std::vector<std::string> allowed_read_paths;
    std::vector<std::string> allowed_write_paths;
    std::vector<int> allowed_tcp_ports;
    bool network = true; // ADR-015: true means network namespace is shared
};

} // namespace agentos
