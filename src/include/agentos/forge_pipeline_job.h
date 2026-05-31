#pragma once

#include <string>
#include <vector>

namespace agentos {

struct ForgePipelineJob {
    std::string id;                     // forge pipeline job UUID
    std::string task_id;                // triggering task id
    std::string status;                 // "draft","review","approved","promoted","rejected","human_review"
    std::string requirement_json;       // JSON describing worker capability requirement
    std::string writer_output_json;     // Code Writer's JSON response
    std::string reviewer_verdict_json;  // Code Reviewer's JSON response (accept/reject + reason)
    std::string feedback;               // feedback for next attempt (empty on first attempt)
    int         attempt = 0;
    int         max_attempts = 3;
    std::string last_code_path;         // path to saved code file (attempt_N.py)
    std::string created_at;             // ISO‑8601 timestamp
    std::string updated_at;             // ISO‑8601 timestamp
    std::string result;                 // optional final result / error message
};

} // namespace agentos
