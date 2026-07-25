#include "agentos/memory_curve.h"

namespace agentos {

// new_score = old_score * (1 - alpha) + signal * alpha
//
// alpha controls how fast old signal fades: closer to 1 means the most
// recent signal dominates almost immediately; closer to 0 means many
// repeated signals are needed to move the score meaningfully.
//
// On the first signal for a given (user_id, fact_type, fact_key) —
// old_score is nullopt — there is nothing to blend against, so the score
// is seeded with the raw signal itself, not blended against an arbitrary
// default like 0.5.
//
// Pure function per ADR-036's determinism requirement: no clock reads,
// no I/O, nothing but old_score/signal/params feeds the result.
double ema_update(const ScoreUpdateInput& input, const rapidjson::Value& params) {
    const double alpha = params["alpha"].GetDouble();
    if (!input.old_score.has_value()) {
        return input.signal;
    }
    return (*input.old_score) * (1.0 - alpha) + input.signal * alpha;
}

}  // namespace agentos
