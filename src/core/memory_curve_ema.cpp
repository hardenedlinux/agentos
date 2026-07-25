#include "agentos/memory_curve.h"

namespace agentos {
namespace {

double ema_update(const ScoreUpdateInput& input,
                  const rapidjson::Value& params) {
    const double alpha = params["alpha"].GetDouble();
    if (!input.old_score.has_value()) {
        return input.signal;
    }
    return (*input.old_score) * (1.0 - alpha) + input.signal * alpha;
}

}  // namespace

struct EmaRegistrar {
    EmaRegistrar() {
        MemoryCurveRegistry::instance().register_algorithm("ema", &ema_update);
    }
};
static EmaRegistrar _ema_registrar;

}  // namespace agentos
