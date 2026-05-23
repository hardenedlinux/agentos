#pragma once

#include <string>
#include <vector>

namespace agentos {

/// Represents a single forge job (a candidate worker that goes through
/// draft → review → sandbox → approve → promote lifecycle).
struct ForgeJob {
    std::string id;
    std::string status;          // "draft", "review", "sandbox", "approved", "promoted", "rejected"
    std::string goal;            // high‑level description of what the worker should do
    std::string created_at;      // ISO‑8601 timestamp
    std::string updated_at;      // ISO‑8601 timestamp
    std::string result;          // optional result (e.g., error message or output)
};

} // namespace agentos
